# Выставляет +x на файл, переданный в -DFILE=... . Вызывается из POST_BUILD через cmake -P:
# команды "cmake -E chmod" не существует, а copy_if_different права не сохраняет.
if(NOT DEFINED FILE)
    message(FATAL_ERROR "chmod_7za.cmake: FILE is not set")
endif()
if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "chmod_7za.cmake: file not found: ${FILE}")
endif()
file(CHMOD "${FILE}"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                 GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)
