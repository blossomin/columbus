# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.13

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake3

# The command to remove a file.
RM = /usr/bin/cmake3 -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/ec2-user/CSE227/functions/cpp

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/ec2-user/CSE227/functions/cpp/build

# Utility rule file for aws-lambda-package-hello.

# Include the progress variables for this target.
include CMakeFiles/aws-lambda-package-hello.dir/progress.make

CMakeFiles/aws-lambda-package-hello: hello
	/home/ec2-user/out/lib/aws-lambda-runtime/cmake/packager /home/ec2-user/CSE227/functions/cpp/build/hello

aws-lambda-package-hello: CMakeFiles/aws-lambda-package-hello
aws-lambda-package-hello: CMakeFiles/aws-lambda-package-hello.dir/build.make

.PHONY : aws-lambda-package-hello

# Rule to build all files generated by this target.
CMakeFiles/aws-lambda-package-hello.dir/build: aws-lambda-package-hello

.PHONY : CMakeFiles/aws-lambda-package-hello.dir/build

CMakeFiles/aws-lambda-package-hello.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/aws-lambda-package-hello.dir/cmake_clean.cmake
.PHONY : CMakeFiles/aws-lambda-package-hello.dir/clean

CMakeFiles/aws-lambda-package-hello.dir/depend:
	cd /home/ec2-user/CSE227/functions/cpp/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/ec2-user/CSE227/functions/cpp /home/ec2-user/CSE227/functions/cpp /home/ec2-user/CSE227/functions/cpp/build /home/ec2-user/CSE227/functions/cpp/build /home/ec2-user/CSE227/functions/cpp/build/CMakeFiles/aws-lambda-package-hello.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/aws-lambda-package-hello.dir/depend

