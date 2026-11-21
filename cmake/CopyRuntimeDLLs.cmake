# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

# An executable with only static dependencies has an empty runtime DLL list.
if(SNR_RUNTIME_DLLS)
    # Parallel executable links can stage the same DLL into one directory.
    # Serialize the copies so Windows never sees competing writes to it.
    file(LOCK "${SNR_RUNTIME_DESTINATION}/snrender-runtime-dlls.lock" GUARD PROCESS)
endif()
foreach(dll IN LISTS SNR_RUNTIME_DLLS)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${dll}" "${SNR_RUNTIME_DESTINATION}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
