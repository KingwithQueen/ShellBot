/* Raw HTTP access logs are disabled; future audit records belong to the core. */
static void log_access(const struct mg_connection *connection) {
    (void)connection;
}
