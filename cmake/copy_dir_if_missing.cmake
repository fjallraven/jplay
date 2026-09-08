# Copies the contents of SRC into DST, but only if DST does not already exist.
# Used to bundle the Python stdlib next to the exe without paying the full
# directory copy on every build. Delete the DST folder to force a refresh
# (e.g. after upgrading Python).
if(NOT EXISTS "${DST}")
    message(STATUS "Bundling: ${SRC} -> ${DST}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SRC}" "${DST}")
else()
    message(STATUS "Already bundled, skipping: ${DST}")
endif()
