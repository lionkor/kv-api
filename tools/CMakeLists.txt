add_executable(sleipnir sleipnir.cpp)

target_link_libraries(sleipnir ${PRJ_LIBRARIES})
target_compile_features(sleipnir PRIVATE ${PRJ_COMPILE_FEATURES})
target_compile_definitions(sleipnir PRIVATE ${PRJ_DEFINITIONS} ${PRJ_WARNINGS})
set_project_warnings(sleipnir)
