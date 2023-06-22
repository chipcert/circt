echo CIRCT_SRC_DIR=${CIRCT_SRC_DIR:=$PWD}
echo INSTALL_PREFIX=${INSTALL_PREFIX:=$CIRCT_SRC_DIR/../circt-bin}
echo CIRCT_CMAKE_BUILD_DIR=${CIRCT_CMAKE_BUILD_DIR:=$CIRCT_SRC_DIR/build}
export CIRCT_CMAKE_BUILD_DIR
echo CMAKE_GENERATOR=${CMAKE_GENERATOR:=Ninja}
export CMAKE_GENERATOR
echo CIRCT_LLVM_DIR=${CIRCT_LLVM_DIR:=$CIRCT_SRC_DIR/llvm/llvm}
export CIRCT_LLVM_DIR
echo CIRCT_PYTHON=${CIRCT_PYTHON:=OFF}
echo CMAKE_C_COMPILER_LAUNCHER=$CMAKE_C_COMPILER_LAUNCHER
echo CMAKE_CXX_COMPILER_LAUNCHER=$CMAKE_CXX_COMPILER_LAUNCHER
echo CIRCT_RELEASE_TAG=${CIRCT_RELEASE_TAG:=chipsorcery}

cmake -B $CIRCT_CMAKE_BUILD_DIR -G $CMAKE_GENERATOR $CIRCT_LLVM_DIR \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DLLVM_TARGETS_TO_BUILD=host \
    -DLLVM_ENABLE_PROJECTS=mlir \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DLLVM_EXTERNAL_PROJECTS=circt \
    -DLLVM_EXTERNAL_CIRCT_SOURCE_DIR=$CIRCT_SRC_DIR \
    -DLLVM_STATIC_LINK_CXX_STDLIB=ON \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    -DLLVM_INSTALL_UTILS=OFF \
    -DMLIR_ENABLE_BINDINGS_PYTHON=$CIRCT_PYTHON \
    -DMLIR_INSTALL_AGGREGATE_OBJECTS=OFF \
    -DCIRCT_ENABLE_FRONTENDS=ON \
    -DCIRCT_INCLUDE_DOCS=OFF \
    -DCIRCT_BINDINGS_PYTHON_ENABLED=$CIRCT_PYTHON \
    -DCIRCT_RELEASE_TAG=$CIRCT_RELEASE_TAG \
    -DCIRCT_RELEASE_TAG_ENABLED=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DLLVM_BUILD_EXAMPLES=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_ENABLE_OCAMLDOC=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF \
    -DLLVM_CCACHE_BUILD=ON \
    -DLLVM_BUILD_TOOLS=OFF \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_LINK_LLVM_DYLIB=OFF

cmake --build $CIRCT_CMAKE_BUILD_DIR

if [ -n INSTALL ]; then
    cmake --build $CIRCT_CMAKE_BUILD_DIR --target install
fi

#pip install $CIRCT_SRC_DIR/lib/Bindings/Python

