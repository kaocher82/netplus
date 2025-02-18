MK_GENERIC_DIR 		:= $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
CURRENT_DIR 		:= $(shell pwd)
UNAME_S 			:= $(shell uname -s)
OS_NAME				:= $(shell hostnamectl status| grep -i 'Operating System' | awk '{print $$3}')
PRJ_BUILD			:= release
PRJ_ARCH			:= x86_64
PRJ_BUILD_SUFFIX 	:= 
PRJ_DEFS			:= 
PRJ_COMM			:=NO


#
# usage
# make build=debug arch=x86_32 comm=yes
# make build=release arch=x86_64 comm=yes
#

#build_config could be [release|debug]
ifdef build
	PRJ_BUILD := $(build)
endif


ifdef arch
	PRJ_ARCH :=$(arch)
endif

ifdef comm
	PRJ_COMM := $(comm)
endif


ifdef defs
	PRJ_DEFS := $(defs)
endif

DEFINES := $(DEFINES) $(PRJ_DEFS)

ifeq ($(PRJ_ARCH),armv7a)
	CXX := armv7-rpi2-linux-gnueabihf-g++
	CC := armv7-rpi2-linux-gnueabihf-gcc
	AR := armv7-rpi2-linux-gnueabihf-ar
endif

#detect cxx version
$(shell $(CXX) -v 2> ./__tmp_makefile_cxx_version)
CXX_VERSION_WITH_PATCH := $(shell cat ./__tmp_makefile_cxx_version | grep -i 'gcc version' | awk '{print $$3}')
$(shell rm -f ./__tmp_makefile_cxx_version)

CXX_VERSION_MAJOR := $(shell echo $(CXX_VERSION_WITH_PATCH) | awk -F. '{print $$1}')
CXX_VERSION_MINOR := $(shell echo $(CXX_VERSION_WITH_PATCH) | awk -F. '{print $$2}')
CXX_VERSION := $(CXX_VERSION_MAJOR).$(CXX_VERSION_MINOR)

ifdef deps
	PRJ_DEPS := $(deps)
else
	PRJ_DEPS := $(MK_GENERIC_DIR)/../../../netplus_deps
endif

DEPS_LIBS_CONFIG_INC_FILE 	:= $(PRJ_DEPS)/deps-libs-config.inc
DEPS_LIBS_DEFS_FILE		 	:= $(PRJ_DEPS)/deps-libs-defs

DEPS_INC 	:= 
DEPS_LIB 	:= 
DEPS_DEFS 	:= 

ifneq ("$(wildcard $(DEPS_LIBS_CONFIG_INC_FILE))","")
	include $(DEPS_LIBS_CONFIG_INC_FILE)
	DEPS_INC := $(DEPS_INC) $(_DEPS_INC)
	DEPS_LIB := $(DEPS_LIB) $(_DEPS_LIB)
endif

ifneq ("$(wildcard $(DEPS_LIBS_DEFS_FILE))","")
	DEPS_DEFS 	:= $(shell cat $(DEPS_LIBS_DEFS_FILE))
	DEFINES 	:= $(DEPS_DEFS) $(DEFINES)
endif


CC_INC 		:= $(DEPS_INC) $(CC_INC)
LINK_LIBS 	:= $(DEPS_LIB) $(LINK_LIBS)

CC_SIMD = 
CC_3RD_CPP_MISC = 

#preprocessing related flag, it's useful for debug purpose
#refer to https://gcc.gnu.org/onlinedocs/gcc-8.3.0/gcc/Preprocessor-Options.html#Preprocessor-Options
#-MP -MMD -MF dependency_file

#-fPIC https://gcc.gnu.org/onlinedocs/gcc-8.3.0/gcc/Code-Gen-Options.html#Code-Gen-Options
CC_MISC		:= -fPIC -c
CC_C11		:= -std=c++11
LINK_MISC 	:= -rdynamic $(LINK_MISC)
LINK_CXX_LIBS = -lpthread -ldl

ifneq ($(UNAME_S),Darwin)
	LINK_CXX_LIBS := -lrt $(LINK_CXX_LIBS)
endif

LINK_LIBS := $(LINK_LIBS) $(LINK_CXX_LIBS)

ifeq ($(PRJ_BUILD),debug)
	PRJ_BUILD_SUFFIX := d
	DEFINES := $(DEFINES) DEBUG
	CC_MISC := $(CC_MISC) -g -Wall -O0
else
	DEFINES := $(DEFINES) RELEASE NDEBUG
	CC_MISC := $(CC_MISC) -O2
endif

#-ftree-vectorize enable this option would result bus error for rpi4

ifeq ($(PRJ_ARCH),x86_64)
    CC_MISC := $(CC_MISC) -m64
	LINK_MISC := -m64 $(LINK_MISC)
else ifeq ($(PRJ_ARCH),x86_32)
    CC_MISC := $(CC_MISC) -m32
	LINK_MISC := -m32 $(LINK_MISC)
else ifeq ($(PRJ_ARCH),armv7a)
    CC_MISC := $(CC_MISC)
else 
	CC_MISC := $(CC_MISC) -munknown_arch
endif

X86_X86_X86 := x86_32 x86_64
ARCH_IS_X86 := YES
ARCH_IS_ARMV7A := NO
SIMD_DEFINES := 

#for x86_64, sse2 is on by default for both x32&x64 gcc compiler
#for x86_32, sse2 is off by default for a x32 gcc compiler
ifeq ($(PRJ_ARCH), $(findstring $(PRJ_ARCH),$(X86_X86_X86) ))
	
		SIMD_DEFINES 	:= NETP_ENABLE_SSE2 NETP_ENABLE_SSE3 NETP_ENABLE_SSSE3 NETP_ENABLE_SSE4_1 NETP_ENABLE_AVX NETP_ENABLE_AVX2 NETP_ENABLE_BMI
		CC_SIMD_SSE2 	:= 	-msse2
		CC_SIMD_SSSE3 	:= 	$(CC_SIMD_SSE2) -msse3 -mssse3
		CC_SIMD_SSE4_1 	:= 	$(CC_SIMD_SSSE3) -msse4.1
		CC_SIMD_AVX 	:= 	$(CC_SIMD_SSE4_1) -mavx
		CC_SIMD_AVX2 	:= 	$(CC_SIMD_AVX) -mavx2 -mbmi

	
else ifeq ($(PRJ_ARCH),armv7a)
	CC_SIMD_NEON := -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon -fno-tree-vectorize

	SIMD_DEFINES := NETP_ENABLE_NEON
	ARCH_IS_X86 := NO
	ARCH_IS_ARMV7A := YES
else 
	ARCH_IS_X86 := NO
endif

SIMD_DEFINES :=\
	$(foreach define,$(SIMD_DEFINES), -D$(define))


ifdef ver
	TARGET_VER := $(ver)
else
	TARGET_VER := a000
endif

CC_DUMP := NO

ifdef cc_dump
	CC_DUMP := $(cc_dump)
endif


comma:=,
empty:=
space:=$(empty) $(empty)

ifneq ($(PRJ_SIMD),)
	ARCH_BUILD_NAME := $(PRJ_ARCH)_$(PRJ_SIMD)
else
	ARCH_BUILD_NAME := $(PRJ_ARCH)
endif

ifneq ($(PRJ_BUILD_SUFFIX),)
	ARCH_BUILD_NAME := $(ARCH_BUILD_NAME)_$(PRJ_BUILD_SUFFIX)
endif


LIBPREFIX	= lib
LIBEXT		= a
ifndef $(O_EXT)
	O_EXT=o
endif