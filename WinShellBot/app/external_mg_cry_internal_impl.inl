/* CivetWeb's supported logging hook for NO_FILESYSTEMS. No raw URLs,
   credentials or request bodies are emitted by the embedded HTTP library. */
static void mg_cry_internal_impl(const struct mg_connection *connection,
                                 const char *function, unsigned line,
                                 const char *format, va_list arguments) {
    (void)connection;
    (void)function;
    (void)line;
    (void)format;
    (void)arguments;
}
