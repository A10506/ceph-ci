function(build_google_test gtest_root)
  include(ExternalProject)
  ExternalProject_Add(googlemock-ext
    SOURCE_DIR ${gtest_root}
    CMAKE_ARGS
      -DBUILD_GMOCK=ON
      -DBUILD_GTEST=OFF
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    INSTALL_COMMAND "")

  set(GTest_INCLUDE_DIRS
    ${gtest_root}/googletest/include)
  set(GMock_INCLUDE_DIRS
    ${gtest_root}/googlemock/include
    ${gtest_root}/googletest/include)

  ExternalProject_Get_Property(googlemock-ext binary_dir)
  set(GMock_GMock_LIBRARY ${binary_dir}/googlemock/libgmock.a)
  set(GMock_Main_LIBRARY ${binary_dir}/googlemock/libgmock_main.a)
  set(GTest_GTest_LIBRARY ${binary_dir}/googlemock/gtest/libgtest.a)
  set(GTest_Main_LIBRARY ${binary_dir}/googlemock/gtest/libgtest_main.a)

  foreach(pkg GTest GMock)
    foreach(c ${pkg} Main)
      set(gtest_library "${pkg}::${c}")
      add_library(${gtest_library} STATIC IMPORTED)
      set_target_properties(${gtest_library} PROPERTIES
        IMPORTED_LOCATION "${${pkg}_${c}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${${pkg}_INCLUDE_DIRS}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        IMPORTED_LINK_INTERFACE_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
      add_dependencies(${gtest_library} googlemock-ext)
    endforeach()
  endforeach()
endfunction()
