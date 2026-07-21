# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "CMakeFiles/clashauto-helper_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/clashauto-helper_autogen.dir/ParseCache.txt"
  "CMakeFiles/clashauto-qml_autogen.dir/AutogenUsed.txt"
  "CMakeFiles/clashauto-qml_autogen.dir/ParseCache.txt"
  "clashauto-helper_autogen"
  "clashauto-qml_autogen"
  )
endif()
