PACKAGE_CHANGELOG="-D CPACK_RPM_CHANGELOG_FILE="
PACKAGE_FILE_NAME="-D CPACK_PACKAGE_FILE_NAME=uca-plugin-andor--0.0.1-1"
PACKAGE_NAME="-D CPACK_PACKAGE_NAME=uca-plugin-andor"
PACKAGE_DEPENDENCIES="-D CPACK_RPM_PACKAGE_REQUIRES=\"libuca >= 1.2.0\""
PACKAGE_DESCRIPTION="-D CPACK_RPM_PACKAGE_DESCRIPTION=\"\""
PACKAGE_RELEASE="-D CPACK_RPM_PACKAGE_RELEASE=1"
PACKAGE_SUMMARY="-D CPACK_RPM_PACKAGE_SUMMARY=\"Andor plugin for libuca\""
PACKAGE_VENDOR="-D CPACK_RPM_PACKAGE_VENDOR=\"\""
PACKAGE_VERSION="-D CPACK_PACKAGE_VERSION=0.0.1"

FLAGS=" \
       $PACKAGE_CHANGELOG \
       $PACKAGE_DEPENDENCIES \
       $PACKAGE_DESCRIPTION \
       $PACKAGE_FILE_NAME \
       $PACKAGE_NAME \
       $PACKAGE_RELEASE \
       $PACKAGE_SUMMARY \
       $PACKAGE_VENDOR \
       $PACKAGE_VERSION \
       "

cmd="cpack -D CPACK_INSTALL_CMAKE_PROJECTS=\"/home/werchner/HIWI/andor/libuca/libuca/plugins/andor/andor/;Project;andor;/\" $FLAGS"

eval $cmd