#!/bin/bash

set -e
set -x

THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR=${THIS_DIR}/../

# setup temp dirs
BUILD_DIR=$(mktemp -d)
DOCS_DIR=$(mktemp -d)
INSTALL_DIR=$(mktemp -d)
DB_DIR=$(mktemp -d)
trap "rm -rf ${DB_DIR} ${BUILD_DIR} \
  ${DOCS_DIR} ${INSTALL_DIR}" EXIT

source /etc/os-release
case $ID in
  centos|fedora)
    case $(lsb_release -si) in
      CentOS)
        MAJOR_VERSION=$(lsb_release -rs | cut -f1 -d.)
        if test $(lsb_release -si) = CentOS -a $MAJOR_VERSION = 7 ; then
          source /opt/rh/devtoolset-7/enable
        fi
        ;;
    esac
esac

# build documentation
${ROOT_DIR}/doc/build.sh ${DOCS_DIR}
test -r ${DOCS_DIR}/output/html/index.html

# build and install
CMAKE_BUILD_TYPE=Debug
if [ "${RUN_COVERAGE}" == 1 ]; then
  CMAKE_BUILD_TYPE=Coverage
fi

CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} \
             -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR}"
if [[ "$OSTYPE" != "darwin"* ]]; then
  CMAKE_FLAGS="${CMAKE_FLAGS} -DWITH_JNI=ON"
fi

pushd ${BUILD_DIR}
cmake ${CMAKE_FLAGS} ${ROOT_DIR}
make -j$(nproc)
make install
popd

PATH=${INSTALL_DIR}/bin:$PATH

# list of tests to run
tests="cruzdb_test_db"

for test_runner in $tests; do
  ${test_runner}
  if [ "${RUN_COVERAGE}" == 1 ]; then
    pushd ${BUILD_DIR}
    make ${test_runner}_coverage || (popd && continue)
    rm -rf coverage*
    lcov --directory . --capture --output-file coverage.info
    lcov --remove coverage.info '/usr/*' '*/googletest/*' '*.pb.cc' '*.pb.h' --output-file coverage2.info
    bash <(curl -s https://codecov.io/bash) -R ${ROOT_DIR} -f coverage2.info || \
      echo "Codecov did not collect coverage reports"
    popd
  fi
done

if [[ "$OSTYPE" != "darwin"* ]]; then
  pushd ${BUILD_DIR}/src/java

  export LD_LIBRARY_PATH=${INSTALL_DIR}/lib:${INSTALL_DIR}/lib64:${INSTALL_DIR}/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

  # i'm giving up for the time being on how to fix a dynamic library loading
  # issue that is only showing up on debian jessie. see issue #143
  OS_ID=$(lsb_release -si)
  OS_CODE=$(lsb_release -sc)
  if [[ ${OS_ID} == "Debian" && ${OS_CODE} == "jessie" ]]; then
    export LD_LIBRARY_PATH=/usr/lib/jvm/java-7-openjdk-amd64/jre/lib/amd64/xawt/:$LD_LIBRARY_PATH
  fi

  export CP=${INSTALL_DIR}/share/java/cruzdb.jar
  export CP=${CP}:${INSTALL_DIR}/share/java/cruzdb-test.jar
  export CP=${CP}:/usr/share/java/zlog.jar
  export CP=${CP}:${BUILD_DIR}/src/java/test-libs/junit-4.12.jar
  export CP=${CP}:${BUILD_DIR}/src/java/test-libs/hamcrest-core-1.3.jar
  export CP=${CP}:${BUILD_DIR}/src/java/test-libs/assertj-core-1.7.1.jar

  mkdir db
  java -cp $CP org.junit.runner.JUnitCore org.cruzdb.AllTests

  popd
fi
