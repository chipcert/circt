//===- EmitVerilog.cpp - Verilog Emitter ----------------------------------===//
//
// This is the main Verilog emitter implementation.
//
//===----------------------------------------------------------------------===//

#include "spt/EmitVerilog.h"
#include "mlir/IR/Module.h"
#include "mlir/Translation.h"
#include "spt/Dialect/FIRRTL/Visitors.h"
#include "spt/Support/LLVM.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

using namespace spt;
using namespace firrtl;
using namespace mlir;

//===----------------------------------------------------------------------===//
// Helper routines
//===----------------------------------------------------------------------===//

/// Return the width of the specified FIRRTL type in bits or -1 if it isn't
/// supported.
static int getBitWidthOrSentinel(FIRRTLType type) {
  switch (type.getKind()) {
  case FIRRTLType::Clock:
  case FIRRTLType::Reset:
  case FIRRTLType::AsyncReset:
    return 1;

  case FIRRTLType::SInt:
  case FIRRTLType::UInt:
    return type.cast<IntType>().getWidthOrSentinel();

  default:
    return -1;
  };
}

/// Given an integer value, return the number of characters it will take to
/// print its base-10 value.
static unsigned getPrintedIntWidth(unsigned value) {
  if (value < 10)
    return 1;
  if (value < 100)
    return 2;
  if (value < 1000)
    return 3;

  SmallVector<char, 8> spelling;
  llvm::raw_svector_ostream stream(spelling);
  stream << value;
  return stream.str().size();
}

/// Return true if this expression should be emitted inline into any statement
/// that uses it.
static bool isExpressionEmittedInline(Operation *op) {
  // ConstantOp is always emitted inline.
  if (isa<ConstantOp>(op) || isa<SubfieldOp>(op))
    return true;

  // Otherwise, if it has multiple uses, emit it out of line.
  return op->getResult(0).hasOneUse();
}

/// This represents a flattened bundle field element.
struct FlatBundleFieldEntry {
  /// This is the underlying ground type of the field.
  FIRRTLType type;
  /// This is a suffix to add to the field name to make it unique.
  std::string suffix;
  /// This indicates whether the field was flipped to be an output.
  bool isOutput;
};

/// Convert a nested bundle of fields into a flat list of fields.  This is used
/// when working with instances and mems to flatten them.
static void flattenBundleTypes(FIRRTLType type, StringRef suffixSoFar,
                               bool isFlipped,
                               SmallVectorImpl<FlatBundleFieldEntry> &results) {
  if (auto flip = type.dyn_cast<FlipType>())
    return flattenBundleTypes(flip.getElementType(), suffixSoFar, !isFlipped,
                              results);

  // In the base case we record this field.
  auto bundle = type.dyn_cast<BundleType>();
  if (!bundle) {
    results.push_back({type, suffixSoFar.str(), isFlipped});
    return;
  }

  SmallString<16> tmpSuffix(suffixSoFar);

  // Otherwise, we have a bundle type.  Break it down.
  for (auto &elt : bundle.getElements()) {
    // Construct the suffix to pass down.
    tmpSuffix.resize(suffixSoFar.size());
    tmpSuffix.push_back('_');
    tmpSuffix.append(elt.first.strref());
    // Recursively process subelements.
    flattenBundleTypes(elt.second, tmpSuffix, isFlipped, results);
  }
}

//===----------------------------------------------------------------------===//
// VerilogEmitter
//===----------------------------------------------------------------------===//

namespace {
/// This class maintains the mutable state that cross-cuts and is shared by the
/// various emitters.
class VerilogEmitterState {
public:
  explicit VerilogEmitterState(raw_ostream &os) : os(os) {}

  /// The stream to emit to.
  raw_ostream &os;

  bool encounteredError = false;
  unsigned currentIndent = 0;

private:
  VerilogEmitterState(const VerilogEmitterState &) = delete;
  void operator=(const VerilogEmitterState &) = delete;
};
} // namespace

namespace {

/// This is the base class for all of the Verilog Emitter components.
class VerilogEmitterBase {
public:
  explicit VerilogEmitterBase(VerilogEmitterState &state)
      : state(state), os(state.os) {}

  InFlightDiagnostic emitError(Operation *op, const Twine &message) {
    state.encounteredError = true;
    return op->emitError(message);
  }

  InFlightDiagnostic emitOpError(Operation *op, const Twine &message) {
    state.encounteredError = true;
    return op->emitOpError(message);
  }

  raw_ostream &indent() { return os.indent(state.currentIndent); }

  void addIndent() { state.currentIndent += 2; }
  void reduceIndent() { state.currentIndent -= 2; }

  // All of the mutable state we are maintaining.
  VerilogEmitterState &state;

  /// The stream to emit to.
  raw_ostream &os;

private:
  VerilogEmitterBase(const VerilogEmitterBase &) = delete;
  void operator=(const VerilogEmitterBase &) = delete;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// ModuleEmitter
//===----------------------------------------------------------------------===//

namespace {

class ModuleEmitter : public VerilogEmitterBase {
public:
  explicit ModuleEmitter(VerilogEmitterState &state)
      : VerilogEmitterBase(state) {}

  void emitFModule(FModuleOp module);

  // Expressions

  void emitExpression(Value exp, bool forceRootExpr = false);

  // Statements.
  void emitStatementExpression(Operation *op);
  void emitStatement(ConnectOp op);
  void emitStatement(NodeOp op);
  void emitStatement(InstanceOp op);
  void emitStatement(SkipOp op) {}
  void emitOperation(Operation *op);

  void collectNamesEmitDecls(Block &block);
  void addName(Value value, StringRef name);
  void addName(Value value, StringAttr nameAttr) {
    addName(value, nameAttr ? nameAttr.getValue() : "");
  }

  StringRef getName(Value value) {
    auto *entry = nameTable[value];
    assert(entry && "value expected a name but doesn't have one");
    return entry->getKey();
  }

  llvm::StringSet<> usedNames;
  llvm::DenseMap<Value, llvm::StringMapEntry<llvm::NoneType> *> nameTable;
  size_t nextGeneratedNameID = 0;
};

} // end anonymous namespace

/// Add the specified name to the name table, auto-uniquing the name if
/// required.  If the name is empty, then this creates a unique temp name.
void ModuleEmitter::addName(Value value, StringRef name) {
  if (name.empty())
    name = "_T";

  // Check to see if this name is valid.  The first character cannot be a
  // number of other weird thing.  If it is, start with an underscore.
  if (!isalpha(name.front()) && name.front() != '_') {
    SmallString<16> tmpName("_");
    tmpName += name;
    return addName(value, tmpName);
  }

  auto isValidVerilogCharacter = [](char ch) -> bool {
    return isalpha(ch) || isdigit(ch) || ch == '_';
  };

  // Check to see if the name consists of all-valid identifiers.  If not, we
  // need to escape them.
  for (char ch : name) {
    if (isValidVerilogCharacter(ch))
      continue;

    // Otherwise, we need to escape it.
    SmallString<16> tmpName;
    for (char ch : name) {
      if (isValidVerilogCharacter(ch))
        tmpName += ch;
      else if (ch == ' ')
        tmpName += '_';
      else {
        tmpName += llvm::utohexstr((unsigned char)ch);
      }
    }
    return addName(value, tmpName);
  }

  // Check to see if this name is available - if so, use it.
  auto insertResult = usedNames.insert(name);
  if (insertResult.second) {
    nameTable[value] = &*insertResult.first;
    return;
  }

  // If not, we need to auto-unique it.
  SmallVector<char, 16> nameBuffer(name.begin(), name.end());
  nameBuffer.push_back('_');
  auto baseSize = nameBuffer.size();

  // Try until we find something that works.
  while (1) {
    auto suffix = llvm::utostr(nextGeneratedNameID++);
    nameBuffer.append(suffix.begin(), suffix.end());

    insertResult =
        usedNames.insert(StringRef(nameBuffer.data(), nameBuffer.size()));
    if (insertResult.second) {
      nameTable[value] = &*insertResult.first;
      return;
    }

    // Chop off the suffix and try again.
    nameBuffer.resize(baseSize);
  }
}

//===----------------------------------------------------------------------===//
// Expression Emission
//===----------------------------------------------------------------------===//

namespace {
/// This enum keeps track of the precedence level of various binary operators,
/// where a lower number binds tighter.
enum VerilogPrecedence {
  // Normal precedence levels.
  Unary,      // Symbols and all the unary operators.
  Multiply,   // * , / , %
  Addition,   // + , -
  Shift,      // << , >>
  Comparison, // > , >= , < , <=
  Equality,   // == , !=
  And,        // &
  Xor,        // ^ , ^~
  Or,         // |

  LowestPrecedence,  // Sentinel which is always the lowest precedence.
  ForceEmitMultiUse, // Sentinel saying to recursively emit a multi-used expr.
};

/// This enum keeps track of whether the emitted subexpression is signed or
/// unsigned as seen from the Verilog language perspective.
enum SubExprSignedness { IsSigned, IsUnsigned };

/// This is information precomputed about each subexpression in the tree we
/// are emitting as a unit.
struct SubExprInfo {
  /// The precedence of this expression.
  VerilogPrecedence precedence;

  /// The signedness of the expression.
  SubExprSignedness signedness;

  SubExprInfo(VerilogPrecedence precedence, SubExprSignedness signedness)
      : precedence(precedence), signedness(signedness) {}
};
} // namespace

/// Return the signedness of the specified type.
static SubExprSignedness getSignednessOfValue(Value v) {
  switch (v.getType().getKind()) {
  default:
    assert(0 && "unsupported type");
  case FIRRTLType::Clock:
  case FIRRTLType::Reset:
  case FIRRTLType::AsyncReset:
    return IsUnsigned;
  case FIRRTLType::SInt:
  case FIRRTLType::UInt:
    return v.getType().cast<IntType>().isSigned() ? IsSigned : IsUnsigned;
  }
}

namespace {
/// This builds a recursively nested expression from an SSA use-def graph.  This
/// uses a post-order walk, but it needs to obey precedence and signedness
/// constraints that depend on the behavior of the child nodes.  To handle this,
/// we emit the characters to a SmallVector which allows us to emit a bunch of
/// stuff, then pre-insert parentheses and other things if we find out that it
/// was needed later.
class ExprEmitter : public ExprVisitor<ExprEmitter, SubExprInfo> {
public:
  ExprEmitter(ModuleEmitter &emitter) : emitter(emitter), os(resultBuffer) {}

  void emitExpression(Value exp, bool forceRootExpr, raw_ostream &os);
  friend class ExprVisitor;

private:
  /// Emit the specified value as a subexpression to the stream.
  SubExprInfo emitSubExpr(Value exp, VerilogPrecedence parenthesizeIfLooserThan,
                          bool forceExpectedSign = false);

  SubExprInfo visitUnhandledExpr(Operation *op);

  using ExprVisitor::visitExpr;
  SubExprInfo visitExpr(ConstantOp op);
  SubExprInfo emitBinary(Operation *op, VerilogPrecedence prec,
                         const char *syntax, bool hasStrictSign = false) {
    auto lhsInfo = emitSubExpr(op->getOperand(0), prec, hasStrictSign);
    os << ' ' << syntax << ' ';

    // The precedence of the RHS operand must be tighter than this operator if
    // they have a different opcode in order to handle things like "x-(a+b)".
    // This isn't needed on the LHS, because the relevant Verilog operators are
    // left-associative.
    //
    // Note: this check is a bit conservative, because it isn't ignoring noop
    // casts so they could lead to unnecessary parens.
    auto *rhsOperandOp = op->getOperand(1).getDefiningOp();
    auto rhsPrec = VerilogPrecedence(prec - 1);
    if (rhsOperandOp && op->getName() == rhsOperandOp->getName())
      rhsPrec = prec;

    auto rhsInfo = emitSubExpr(op->getOperand(1), rhsPrec, hasStrictSign);

    // If we have a strict sign, then match the firrtl operation sign.
    // Otherwise, the result is signed if both operands are signed.
    SubExprSignedness signedness;
    if (hasStrictSign)
      signedness = getSignednessOfValue(op->getResult(0));
    else if (lhsInfo.signedness == IsSigned && rhsInfo.signedness == IsSigned)
      signedness = IsSigned;
    else
      signedness = IsUnsigned;

    return {prec, signedness};
  }

  /// Emit the specified subexpression in a context where the sign matters,
  /// e.g. for a less than comparison or divide.
  SubExprInfo emitSignedBinary(Operation *op, VerilogPrecedence prec,
                               const char *syntax) {
    return emitBinary(op, prec, syntax, /*hasStrictSign:*/ true);
  }
  SubExprInfo emitUnary(Operation *op, const char *syntax, bool forceUnsigned) {
    os << syntax;
    auto signedness = emitSubExpr(op->getOperand(0), Unary).signedness;
    return {Unary, forceUnsigned ? IsUnsigned : signedness};
  }
  SubExprInfo emitNoopCast(Operation *op) {
    return emitSubExpr(op->getOperand(0), LowestPrecedence);
  }

  SubExprInfo visitExpr(AddPrimOp op) { return emitBinary(op, Addition, "+"); }
  SubExprInfo visitExpr(SubPrimOp op) { return emitBinary(op, Addition, "-"); }
  SubExprInfo visitExpr(MulPrimOp op) { return emitBinary(op, Multiply, "*"); }
  SubExprInfo visitExpr(DivPrimOp op) {
    return emitSignedBinary(op, Multiply, "/");
  }
  SubExprInfo visitExpr(RemPrimOp op) {
    return emitSignedBinary(op, Multiply, "%");
  }

  SubExprInfo visitExpr(AndPrimOp op) { return emitBinary(op, And, "&"); }
  SubExprInfo visitExpr(OrPrimOp op) { return emitBinary(op, Or, "|"); }
  SubExprInfo visitExpr(XorPrimOp op) { return emitBinary(op, Xor, "^"); }

  // Comparison Operations
  SubExprInfo visitExpr(LEQPrimOp op) {
    return emitSignedBinary(op, Comparison, "<=");
  }
  SubExprInfo visitExpr(LTPrimOp op) {
    return emitSignedBinary(op, Comparison, "<");
  }
  SubExprInfo visitExpr(GEQPrimOp op) {
    return emitSignedBinary(op, Comparison, ">=");
  }
  SubExprInfo visitExpr(GTPrimOp op) {
    return emitSignedBinary(op, Comparison, ">");
  }
  SubExprInfo visitExpr(EQPrimOp op) { return emitBinary(op, Equality, "=="); }
  SubExprInfo visitExpr(NEQPrimOp op) { return emitBinary(op, Equality, "!="); }

  // Cat, DShl, DShr, ValidIf

  // Unary Prefix operators.
  SubExprInfo visitExpr(AndRPrimOp op) { return emitUnary(op, "&", true); }
  SubExprInfo visitExpr(XorRPrimOp op) { return emitUnary(op, "^", true); }
  SubExprInfo visitExpr(OrRPrimOp op) { return emitUnary(op, "|", true); }
  SubExprInfo visitExpr(NotPrimOp op) { return emitUnary(op, "~", false); }

  // Noop cast operators.
  SubExprInfo visitExpr(AsAsyncResetPrimOp op) { return emitNoopCast(op); }
  SubExprInfo visitExpr(AsClockPrimOp op) { return emitNoopCast(op); }

  // Signedness tracks the verilog sign, not the FIRRTL sign, so we don't need
  // to emit anything for AsSInt/AsUInt.  Their results will get casted by the
  // client as necessary.
  SubExprInfo visitExpr(AsUIntPrimOp op) { return emitNoopCast(op); }
  SubExprInfo visitExpr(AsSIntPrimOp op) { return emitNoopCast(op); }

  // Other
  SubExprInfo visitExpr(SubfieldOp op);
  SubExprInfo visitExpr(BitsPrimOp op);
  SubExprInfo visitExpr(ShlPrimOp op);
  SubExprInfo visitExpr(ShrPrimOp op);

private:
  ModuleEmitter &emitter;
  SmallString<128> resultBuffer;
  llvm::raw_svector_ostream os;
};
} // end anonymous namespace

/// Emit the specified value as an expression.  If this is an inline-emitted
/// expression, we emit that expression, otherwise we emit a reference to the
/// already computed name.  If 'forceRootExpr' is true, then this emits an
/// expression even if we typically don't do it inline.
///
void ExprEmitter::emitExpression(Value exp, bool forceRootExpr,
                                 raw_ostream &os) {
  // Emit the expression.
  emitSubExpr(exp, forceRootExpr ? ForceEmitMultiUse : LowestPrecedence);

  // Once the expression is done, we can emit the result to the stream.
  os << resultBuffer;
}

/// Emit the specified value as a subexpression to the stream.
SubExprInfo ExprEmitter::emitSubExpr(Value exp,
                                     VerilogPrecedence parenthesizeIfLooserThan,
                                     bool forceExpectedSign) {
  auto *op = exp.getDefiningOp();
  bool shouldEmitInlineExpr = op && isExpression(op);

  // Don't emit this expression inline if it has multiple uses.
  if (shouldEmitInlineExpr && parenthesizeIfLooserThan != ForceEmitMultiUse &&
      !isExpressionEmittedInline(op))
    shouldEmitInlineExpr = false;

  // If this is a non-expr or shouldn't be done inline, just refer to its
  // name.
  if (!shouldEmitInlineExpr) {
    if (forceExpectedSign && getSignednessOfValue(exp) != IsUnsigned) {
      os << "$signed(" << emitter.getName(exp) << ')';
      return {Unary, IsSigned};
    }
    os << emitter.getName(exp);
    return {Unary, IsUnsigned};
  }

  unsigned subExprStartIndex = resultBuffer.size();

  // Okay, this is an expression we should emit inline.  Do this through our
  // visitor.
  auto expInfo = dispatchExprVisitor(exp.getDefiningOp());

  // Check cases where we have to insert things before the expression now that
  // we know things about it.
  if (forceExpectedSign && getSignednessOfValue(exp) != expInfo.signedness) {
    // If the sign of the result matters and we emitted something with the
    // wrong sign, correct it.
    StringRef cast = expInfo.signedness == IsSigned ? "$unsigned(" : "$signed(";
    resultBuffer.insert(resultBuffer.begin() + subExprStartIndex, cast.begin(),
                        cast.end());
    os << ')';

  } else if (expInfo.precedence > parenthesizeIfLooserThan) {
    // If this subexpression would bind looser than the expression it is bound
    // into, then we need to parenthesize it.  Insert the parentheses
    // retroactively.
    resultBuffer.insert(resultBuffer.begin() + subExprStartIndex, '(');
    os << ')';
  }

  return expInfo;
}

SubExprInfo ExprEmitter::visitExpr(ConstantOp op) {
  auto resType = op.getType().cast<IntType>();
  if (resType.getWidthOrSentinel() == -1)
    return visitUnhandledExpr(op);

  os << resType.getWidth() << '\'';
  if (resType.isSigned())
    os << 's';
  os << 'h';

  SmallString<32> valueStr;
  op.value().toStringUnsigned(valueStr, 16);
  os << valueStr;
  return {Unary, resType.isSigned() ? IsSigned : IsUnsigned};
}

SubExprInfo ExprEmitter::visitExpr(SubfieldOp op) {
  auto prec = emitSubExpr(op.getOperand(), Unary);
  assert(prec.precedence == Unary);
  os << '_' << op.fieldname();
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitExpr(BitsPrimOp op) {
  emitSubExpr(op.getOperand(), Unary);
  os << '[' << op.hi();
  if (op.hi() != op.lo())
    os << ':' << op.lo();
  os << ']';
  return {Unary, IsUnsigned};
}

// TODO(verilog dialect): There is no need to persist shifts. They are
// apparently only needed for width inference.
SubExprInfo ExprEmitter::visitExpr(ShlPrimOp op) {
  // shl(x, 4) ==> {x, 4'h0}
  os << '{';
  emitSubExpr(op.getOperand(), LowestPrecedence);
  os << ", " << op.amount() << "'h0}";
  return {Unary, IsUnsigned};
}

// TODO(verilog dialect): There is no need to persist shifts. They are
// apparently only needed for width inference.
SubExprInfo ExprEmitter::visitExpr(ShrPrimOp op) {
  auto inWidth = op.getOperand().getType().cast<IntType>().getWidthOrSentinel();
  unsigned shiftAmount = op.amount().getLimitedValue();
  if (inWidth == -1 || shiftAmount >= unsigned(inWidth))
    return visitUnhandledExpr(op);

  emitSubExpr(op.getOperand(), Unary);

  os << '[' << (inWidth - 1);
  if (shiftAmount != unsigned(inWidth - 1)) // Emit x[4] instead of x[4:4].
    os << ':' << shiftAmount;
  os << ']';
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitUnhandledExpr(Operation *op) {
  emitter.emitOpError(op, "cannot emit this expression to Verilog");
  os << "<<unsupported expr: " << op->getName().getStringRef() << ">>";
  return {Unary, IsUnsigned};
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

/// Emit the specified value as an expression.  If this is an inline-emitted
/// expression, we emit that expression, otherwise we emit a reference to the
/// already computed name.  If 'forceRootExpr' is true, then this emits an
/// expression even if we typically don't do it inline.
///
void ModuleEmitter::emitExpression(Value exp, bool forceRootExpr) {
  ExprEmitter(*this).emitExpression(exp, forceRootExpr, os);
}

void ModuleEmitter::emitStatementExpression(Operation *op) {
  indent() << "assign " << getName(op->getResult(0)) << " = ";
  emitExpression(op->getResult(0), /*forceRootExpr=*/true);
  os << ";\n";
  // TODO: location information too.
}

void ModuleEmitter::emitStatement(ConnectOp op) {
  indent() << "assign ";
  emitExpression(op.lhs());
  os << " = ";
  emitExpression(op.rhs());
  os << ";\n";
  // TODO: location information too.
}

void ModuleEmitter::emitStatement(NodeOp op) {
  indent() << "assign " << getName(op.getResult());
  os << " = ";
  emitExpression(op.input());
  os << ";\n";
  // TODO: location information too.
}

void ModuleEmitter::emitStatement(InstanceOp op) {
  auto instanceName = getName(op.getResult());
  StringRef defName = op.moduleName();

  // If this is referencing an extmodule with a specified defname, then use
  // the defName from it as the actual module name we reference.  This exists
  // because FIRRTL is not parameterized like verilog is - it introduces
  // redundant extmodule instances to encode different parameter configurations.
  auto moduleIR = op.getParentOfType<CircuitOp>();
  auto referencedModule = moduleIR.lookupSymbol(defName);
  FExtModuleOp referencedExtModule;

  if (!referencedModule)
    emitOpError(op, "could not find mlir node named @" + defName);
  else if ((referencedExtModule = dyn_cast<FExtModuleOp>(referencedModule)))
    if (auto defNameAttr = referencedExtModule.defname())
      defName = defNameAttr.getValue();
  indent() << defName;

  // Print a parameter constant value in a Verilog compatible way.
  auto printParmValue = [&](Attribute value) {
    if (auto intAttr = value.dyn_cast<IntegerAttr>()) {
      os << intAttr.getValue();
    } else if (auto strAttr = value.dyn_cast<StringAttr>()) {
      os << '"';
      llvm::printEscapedString(strAttr.getValue(), os);
      os << '"';
    } else if (auto fpAttr = value.dyn_cast<FloatAttr>()) {
      // TODO: relying on float printing to be precise is not a good idea.
      os << fpAttr.getValueAsDouble();
    } else {
      os << "<<UNKNOWN MLIRATTR: " << value << ">>";
      emitOpError(op, "unknown extmodule parameter value");
    }
  };

  // If this is a parameterized module, then emit the parameters.
  if (referencedExtModule)
    if (auto paramDictOpt = referencedExtModule.parameters()) {
      DictionaryAttr paramDict = paramDictOpt.getValue();
      if (!paramDict.empty()) {
        os << " #(";
        llvm::interleaveComma(paramDict, os, [&](NamedAttribute elt) {
          os << '.' << elt.first << '(';
          printParmValue(elt.second);
          os << ')';
        });
        os << ')';
      }
    }

  os << ' ' << instanceName << " (\n";
  // TODO: location information.

  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenBundleTypes(op.getResult().getType().cast<FIRRTLType>(), "", false,
                     fieldTypes);
  for (auto &elt : fieldTypes) {
    assert(!elt.suffix.empty() && "instance should always return a BundleType");
    bool isLast = &elt == &fieldTypes.back();
    indent() << "  ." << StringRef(elt.suffix).drop_front() << '('
             << instanceName << elt.suffix << (isLast ? ")\n" : "),\n");
  }
  indent() << ");\n";
}

//===----------------------------------------------------------------------===//
// Module Driver
//===----------------------------------------------------------------------===//

void ModuleEmitter::collectNamesEmitDecls(Block &block) {
  // In the first pass, we fill in the symbol table, calculate the max width of
  // the declaration words and the max type width.
  size_t maxDeclNameWidth = 0, maxTypeWidth = 0;
  SmallVector<Value, 16> declsToEmit;

  // Return the word (e.g. "wire") in Verilog to declare the specified thing.
  auto getVerilogDeclWord = [](Value result) -> StringRef {
    // FIXME: what about mems?
    if (isa<RegOp>(result.getDefiningOp()))
      return "reg";
    else
      return "wire";
  };

  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;

  for (auto &op : block) {
    // If the op has no results, then it doesn't produce a name.
    if (op.getNumResults() == 0)
      continue;

    assert(op.getNumResults() == 1 && "firrtl only has single-op results");

    // If the expression is inline, it doesn't need a name.
    if (isExpression(&op) && isExpressionEmittedInline(&op))
      continue;

    Value result = op.getResult(0);

    // Otherwise, it must be an expression or a declaration like a wire.
    addName(result, op.getAttrOfType<StringAttr>("name"));

    // FIXME(verilog dialect): This can cause name collisions, because the base
    // name may be unique but the suffixed names may not be.  The right way to
    // solve this is to change the instances and mems in a new Verilog dialect
    // to use multiple return values, exposing the independent Value's.

    // Determine what kind of thing this is, and how much space it needs.
    maxDeclNameWidth =
        std::max(getVerilogDeclWord(result).size(), maxDeclNameWidth);

    // Flatten the type for processing of each individual element.
    auto type = result.getType().cast<FIRRTLType>();
    fieldTypes.clear();
    flattenBundleTypes(type, "", false, fieldTypes);

    for (const auto &elt : fieldTypes) {
      int bitWidth = getBitWidthOrSentinel(elt.type);
      if (bitWidth == -1) {
        emitError(&op, getName(result))
            << elt.suffix << " has an unsupported verilog type " << type;
        result = Value();
        break;
      }

      if (bitWidth != 1) { // Width 1 is implicit.
        // Add 5 to count the width of the "[:0] ".
        size_t thisWidth = getPrintedIntWidth(bitWidth - 1) + 5;
        maxTypeWidth = std::max(thisWidth, maxTypeWidth);
      }
    }

    // Emit this declaration.
    if (result)
      declsToEmit.push_back(result);
  }

  // Okay, now that we have measured the things to emit, emit the things.
  for (auto result : declsToEmit) {
    auto word = getVerilogDeclWord(result);

    // Flatten the type for processing of each individual element.
    auto type = result.getType().cast<FIRRTLType>();
    fieldTypes.clear();
    flattenBundleTypes(type, "", false, fieldTypes);

    for (const auto &elt : fieldTypes) {
      indent() << word;
      os.indent(maxDeclNameWidth - word.size() + 1);

      // If there are any widths, emit this one.
      if (maxTypeWidth) {
        int bitWidth = getBitWidthOrSentinel(elt.type);
        assert(bitWidth != -1 && "sentinel handled above");

        unsigned emittedWidth = 0;
        if (bitWidth != 1) {
          os << '[' << (bitWidth - 1) << ":0]";
          emittedWidth = getPrintedIntWidth(bitWidth - 1) + 4;
        }

        os.indent(maxTypeWidth - emittedWidth);
      }

      os << getName(result) << elt.suffix << ";\n";
      // TODO: Emit locator info.
    }
  }

  if (!declsToEmit.empty())
    os << '\n';
}

void ModuleEmitter::emitOperation(Operation *op) {
  // Handle expression statements.
  if (isExpression(op)) {
    if (!isExpressionEmittedInline(op))
      emitStatementExpression(op);
    return;
  }

  // Handle statements.
  // TODO: Refactor out to visitors.
  bool isStatement = false;
  TypeSwitch<Operation *>(op).Case<ConnectOp, NodeOp, InstanceOp, SkipOp>(
      [&](auto stmt) {
        isStatement = true;
        this->emitStatement(stmt);
      });
  if (isStatement)
    return;

  // Ignore the region terminator.
  if (isa<DoneOp>(op))
    return;

  emitOpError(op, "cannot emit this operation to Verilog");
}

void ModuleEmitter::emitFModule(FModuleOp module) {
  // Add all the ports to the name table.
  SmallVector<ModulePortInfo, 8> portInfo;
  module.getPortInfo(portInfo);

  size_t nextPort = 0;
  for (auto &port : portInfo)
    addName(module.getArgument(nextPort++), port.first);

  os << "module " << module.getName() << '(';
  if (!portInfo.empty())
    os << '\n';

  // Determine the width of the widest type we have to print so everything
  // lines up nicely.
  bool hasOutputs = false;
  unsigned maxTypeWidth = 0;
  for (auto &port : portInfo) {
    auto portType = port.second;
    if (auto flip = portType.dyn_cast<FlipType>()) {
      portType = flip.getElementType();
      hasOutputs = true;
    }
    int bitWidth = getBitWidthOrSentinel(portType);
    if (bitWidth == -1 || bitWidth == 1)
      continue; // The error case is handled below.

    // Add 4 to count the width of the "[:0] ".
    unsigned thisWidth = getPrintedIntWidth(bitWidth - 1) + 5;
    maxTypeWidth = std::max(thisWidth, maxTypeWidth);
  }

  addIndent();

  // TODO(QoI): Should emit more than one port on a line.
  //  e.g. output [2:0] auto_out_c_bits_opcode, auto_out_c_bits_param,
  //
  size_t portNumber = 0;
  for (auto &port : portInfo) {
    indent();
    // Emit the arguments.
    auto portType = port.second;
    if (auto flip = portType.dyn_cast<FlipType>()) {
      portType = flip.getElementType();
      os << "output";
    } else {
      os << (hasOutputs ? "input " : "input");
    }

    unsigned emittedWidth = 0;

    int bitWidth = getBitWidthOrSentinel(portType);
    if (bitWidth == -1) {
      emitError(module, "parameter '" + port.first.getValue() +
                            "' has an unsupported verilog type ")
          << portType;
    } else if (bitWidth != 1) {
      // Width 1 is implicit.
      os << " [" << (bitWidth - 1) << ":0]";
      emittedWidth = getPrintedIntWidth(bitWidth - 1) + 5;
    }

    if (maxTypeWidth - emittedWidth)
      os.indent(maxTypeWidth - emittedWidth);

    // Emit the name.
    os << ' ' << getName(module.getArgument(portNumber++));
    if (&port != &portInfo.back())
      os << ',';
    else
      os << ");\n";
    os << '\n';
  }

  if (portInfo.empty())
    os << ");\n";

  // Build up the symbol table for all of the values that need names in the
  // module.
  collectNamesEmitDecls(*module.getBodyBlock());

  // Emit the body.
  for (auto &op : *module.getBodyBlock()) {
    emitOperation(&op);
  }

  reduceIndent();

  os << "endmodule\n\n";
}

//===----------------------------------------------------------------------===//
// CircuitEmitter
//===----------------------------------------------------------------------===//

namespace {

class CircuitEmitter : public VerilogEmitterBase {
public:
  explicit CircuitEmitter(VerilogEmitterState &state)
      : VerilogEmitterBase(state) {}

  void emitMLIRModule(ModuleOp module);

private:
  void emitCircuit(CircuitOp circuit);
};

} // end anonymous namespace

void CircuitEmitter::emitCircuit(CircuitOp circuit) {
  for (auto &op : *circuit.getBody()) {
    if (auto module = dyn_cast<FModuleOp>(op)) {
      ModuleEmitter(state).emitFModule(module);
      continue;
    }

    // Ignore the done terminator at the end of the circuit.
    // Ignore 'ext modules'.
    if (isa<DoneOp>(op) || isa<FExtModuleOp>(op))
      continue;

    op.emitError("unknown operation");
  }
}

void CircuitEmitter::emitMLIRModule(ModuleOp module) {
  for (auto &op : *module.getBody()) {
    if (auto circuit = dyn_cast<CircuitOp>(op))
      emitCircuit(circuit);
    else if (!isa<ModuleTerminatorOp>(op))
      op.emitError("unknown operation");
  }
}

LogicalResult spt::emitVerilog(ModuleOp module, llvm::raw_ostream &os) {
  VerilogEmitterState state(os);
  CircuitEmitter(state).emitMLIRModule(module);
  return failure(state.encounteredError);
}

void spt::registerVerilogEmitterTranslation() {
  static TranslateFromMLIRRegistration toVerilog("emit-verilog", emitVerilog);
}
