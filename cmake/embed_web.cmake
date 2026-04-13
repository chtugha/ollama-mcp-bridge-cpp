file(READ "${INPUT}" CONTENT)
file(WRITE "${OUTPUT}"
    "// Auto-generated from web/index.html — do not edit\n"
    "extern const char* const WEB_UI_HTML = R\"OMB_WEB_9F3A("
)
file(APPEND "${OUTPUT}" "${CONTENT}")
file(APPEND "${OUTPUT}" ")OMB_WEB_9F3A\";\n")
