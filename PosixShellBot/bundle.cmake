# Generate the bundle and native icon from the adopted branding, without changing source assets.
set(bundle "${OUTPUT}/ShellBot.app/Contents")
file(MAKE_DIRECTORY "${bundle}/MacOS" "${bundle}/Resources" "${OUTPUT}/ShellBot.iconset")
configure_file("${PROGRAM}" "${bundle}/MacOS/ShellBot" COPYONLY)
configure_file("${SOURCE}/PosixShellBot/Info.plist" "${bundle}/Info.plist" COPYONLY)
configure_file("${SOURCE}/assets/branding/shellbot-v1/shellbot-icon-small.png" "${bundle}/Resources/shellbot-icon-small.png" COPYONLY)
foreach(size 16 32 128 256 512)
  foreach(scale 1 2)
    math(EXPR pixels "${size} * ${scale}")
    if(scale EQUAL 2)
      set(suffix "@2x")
    else()
      set(suffix "")
    endif()
    execute_process(COMMAND /usr/bin/sips -z "${pixels}" "${pixels}"
      "${SOURCE}/assets/branding/shellbot-v1/shellbot-icon.png"
      --out "${OUTPUT}/ShellBot.iconset/icon_${size}x${size}${suffix}.png"
      RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Cannot create native icon: ${error}")
    endif()
  endforeach()
endforeach()
execute_process(COMMAND /usr/bin/iconutil -c icns "${OUTPUT}/ShellBot.iconset" -o "${bundle}/Resources/ShellBot.icns"
  RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot package native icon: ${error}")
endif()
