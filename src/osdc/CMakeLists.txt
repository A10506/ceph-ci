set(osdc_files
  Filer.cc
  ObjectCacher.cc
  Objecter.cc
  Striper.cc)
add_library(osdc STATIC ${osdc_files})
target_link_libraries(osdc dmclock)
if(WITH_LTTNG AND WITH_EVENTTRACE)
  add_dependencies(osdc eventtrace_tp)
endif()
