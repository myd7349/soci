//
// Copyright (C) 2004-2008 Maciej Sobczak, Stephen Hutton
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//

#define SOCI_POSTGRESQL_SOURCE
#include "soci/soci-platform.h"
#include "soci/postgresql/soci-postgresql.h"
#include "soci/session.h"
#include "soci-compiler.h"
#include "soci-cstrtoi.h"

#include <libpq-fe.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <sstream>

// We have 3 cases of handling tcp_user_timeout option:
//
//  - Windows: libpq doesn't handle it at all in all the current versions, but
//    we may implement support for it ourselves, see below.
//  - Linux (or, to be optimistic, any other system defining TCP_USER_TIMEOUT):
//    libpq supports it since v12, but we also want to provide it when using
//    older versions.
//  - Other: there is no support for it in libpq and we can't implement it
//    (although macOS has TCP_RXT_CONNDROPTIME which seems similar and we could
//    try using it if there is interest in it).
//
// Note that, in theory, we could switch to using async libpq API to implement
// it on all platforms, but for now having it only under Linux and Windows is
// enough for our needs.
#ifdef _WIN32
    // Include the headers we need for setsockopt(TCP_MAXRT) used below.
    #include <winsock2.h>
    #include <ws2tcpip.h>

    // Some old MinGW versions don't define TCP_MAXRT, do it ourselves.
    #ifndef TCP_MAXRT
        #define TCP_MAXRT 5
    #endif
#else // !_WIN32
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif // _WIN32/!_WIN32

using namespace soci;
using namespace soci::details;

// Implement this postgresql_result member function here to avoid adding a
// separate source file just for it.
void postgresql_result::clear()
{
    // Notice that it is safe to call PQclear() with NULL pointer, it
    // simply does nothing in this case.
    PQclear(result_);
}

namespace // unnamed
{

// Helper function to set maximum socket timeout under Windows.
//
// Throws if the parameter is not an integer, but silently ignores invalid
// negative timeout values for consistency with libpq behaviour.
//
// Also throws if the timeout couldn't be set.

void set_tcp_user_timeout(int sock, std::string const& timeoutStr)
{
    int timeoutMs = 0;
    if (!cstring_to_integer(timeoutMs, timeoutStr.c_str()))
    {
        std::ostringstream oss;
        oss << "Invalid value for tcp_user_timeout connection option: \""
            << timeoutStr << "\".";

        throw soci_error(oss.str());
    }

    // Zero timeout means system default and so can be just ignored.
    if (timeoutMs == 0)
        return;

    // Negative timeout seems to be just ignored by both libpq (and Linux fails
    // with EINVAL if it's specified), so ignore it here too (note that -1 has
    // a special meaning for Windows and means to turn off the timeout
    // entirely).
    if (timeoutMs < 0)
        return;

#ifdef _WIN32
    // The value is in milliseconds, but we need to convert it to seconds,
    // prefer to round it rather than truncate.
    constexpr int MS_PER_SEC = 1000;

    // We also shouldn't set timeout to 0 (it's not clear what does this do),
    // so always set it to at least 1 second.
    DWORD timeoutSec = (timeoutMs + MS_PER_SEC / 2) / MS_PER_SEC;
    if (timeoutSec == 0)
        timeoutSec = 1;

    if (setsockopt(sock, IPPROTO_TCP, TCP_MAXRT,
                   reinterpret_cast<char const*>(&timeoutSec),
                   sizeof(timeoutSec)) != 0)
    {
        std::ostringstream oss;
        oss << "Failed to set TCP_MAXRT option on the socket: WinSock error "
            << WSAGetLastError() << ".";

        throw soci_error(oss.str());
    }
#elif defined(TCP_USER_TIMEOUT)
    // We can only set this option for an AF_INET, not AF_UNIX, socket.
    sockaddr_storage sa;
    socklen_t saLen = sizeof(sa);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&sa), &saLen) != 0)
    {
        std::ostringstream oss;
        oss << "Failed to get socket address: " << strerror(errno) << ".";

        throw soci_error(oss.str());
    }

    if (sa.ss_family == AF_UNIX)
        return;

    if (setsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT,
                   &timeoutMs, sizeof(timeoutMs)) != 0)
    {
        std::ostringstream oss;
        oss << "Failed to set TCP_USER_TIMEOUT option on the socket: "
            << strerror(errno) << ".";

        throw soci_error(oss.str());
    }
#else
    // Don't do anything and silently ignore this option. This is not great,
    // but consistent with libpq behaviour and it's not clear what else could
    // we do: throwing an exception would seem to be too drastic, but we don't
    // have any "warning" mechanism for reporting this otherwise.
    SOCI_UNUSED(sock);
#endif // _WIN32/other platforms
}

// helper function for hardcoded queries
void hard_exec(postgresql_session_backend & session_backend,
    PGconn * conn, char const * query, char const * errMsg)
{
    postgresql_result(session_backend, PQexec(conn, query)).check_for_errors(errMsg);
}

// helper function to quote a string before sending to PostgreSQL
std::string quote(PGconn * conn, std::string& s)
{
    int error_code;
    std::string retv;
    retv.resize(2 * s.length() + 3);
    retv[0] = '\'';
    size_t len_esc = PQescapeStringConn(conn, const_cast<char *>(retv.data()) + 1, s.c_str(), s.length(), &error_code);
    if (error_code > 0)
    {
        len_esc = 0;
    }
    retv[len_esc + 1] = '\'';
    retv.resize(len_esc + 2);

    return retv;
}

// helper function to collect schemas from search_path
std::vector<std::string> get_schema_names(postgresql_session_backend & session, PGconn * conn)
{
    std::vector<std::string> schema_names;
    postgresql_result search_path_result(session, PQexec(conn, "SHOW search_path"));
    if (search_path_result.check_for_data("search_path doesn't exist"))
    {
        std::string search_path_content;
        if (PQntuples(search_path_result) > 0)
            search_path_content = PQgetvalue(search_path_result, 0, 0);
        if (search_path_content.empty())
            search_path_content = R"("$user", public)"; // fall back to default value

        bool quoted = false;
        std::string schema;
        while (!search_path_content.empty())
        {
            switch (search_path_content[0])
            {
            case '"':
                quoted = !quoted;
                break;
            case ',':
            case ' ':
                if (!quoted)
                {
                    if (search_path_content[0] == ',')
                    {
                        schema_names.push_back(schema);
                        schema = "";
                    }
                    break;
                }
                SOCI_FALLTHROUGH;
            default:
                schema.push_back(search_path_content[0]);
	    }
            search_path_content.erase(search_path_content.begin());
        }
        if (!schema.empty())
            schema_names.push_back(schema);
        for (std::string& schema_name: schema_names)
        {
            if (schema_name == "$user")
            {
                postgresql_result current_user_result(session, PQexec(conn, "SELECT current_user"));
                if (current_user_result.check_for_data("current_user is not defined"))
                {
                    if (PQntuples(current_user_result) > 0)
                    {
                        schema_name = PQgetvalue(current_user_result, 0, 0);
                    }
                }
            }

            // Ensure no bad characters
            schema_name = quote(conn, schema_name);
        }
    }

    return schema_names;
}

// helper function to create a comma separated list of strings
std::string create_list_of_strings(const std::vector<std::string>& strings)
{
    std::ostringstream oss;
    bool first = true;
    for ( const auto& s: strings )
    {
        if ( first )
            first = false;
        else
            oss << ", ";

        oss << s;
    }
    return oss.str();
}

// helper function to create a case list for strings
std::string create_case_list_of_strings(const std::vector<std::string>& list)
{
    std::ostringstream oss;
    for (size_t i = 0; i < list.size(); ++i) {
        oss << " WHEN " << list[i] << " THEN " << i;
    }
    return oss.str();
}

} // namespace unnamed

postgresql_session_backend::postgresql_session_backend(
    connection_parameters const& parameters)
    : statementCount_(0), conn_(0)
{
    single_row_mode_ = false;

    connect(parameters);
}

void postgresql_session_backend::connect(
    connection_parameters const& parameters)
{
    auto params = parameters;
    params.extract_options_from_space_separated_string();

    // Extract SOCI-specific options, i.e. check if they're present and remove
    // them from params to avoid passing them to PQconnectdb() below.
    std::string value;

    // This one is not used by this backend, but can be present in the
    // connection string if we're called from session::reconnect().
    params.extract_option(option_reconnect, value);

    // Notice that we accept both variants only for compatibility.
    char const* name;
    if (params.extract_option("singlerow", value))
        name = "singlerow";
    else if (params.extract_option("singlerows", value))
        name = "singlerows";
    else
        name = nullptr;

    if (name)
    {
        single_row_mode_ = connection_parameters::is_true_value(name, value);
    }

    if (params.extract_option("tracefile", value) && !value.empty())
    {
        const char* mode;
        if (value.front() == '+')
        {
            mode = "a";
            value.erase(0, 1);
        }
        else
        {
            mode = "w";
        }

        traceFile_ = fopen(value.c_str(), mode);
        if (!traceFile_)
        {
            std::ostringstream oss;
            oss << "Cannot open database trace file: \"" << value << "\".";
            throw soci_error(oss.str());
        }
    }

    // Support for tcp_user_timeout option has been added in libpq 12, so we
    // need to take care of it ourselves if we use an older libpq version. We
    // also need to always do it under Windows as no known libpq version has
    // support for this parameter there.
    //
    // Note that if we don't even have PQlibVersion(), it means we're using
    // libpq < 9.1, i.e. definitely less than 12.
    std::string timeoutStr;
#if !defined(_WIN32) && !defined(SOCI_POSTGRESQL_NO_LIBVERSION)
    if ( PQlibVersion() < 120000 )
#endif
    {
        params.extract_option("tcp_user_timeout", timeoutStr);
    }
    // else: We're not under Windows and libpq is recent enough to handle this
    //       connection option itself, so just let it do it.

    // We can't use SOCI connection string with PQconnectdb() directly because
    // libpq uses single quotes instead of double quotes used by SOCI.
    PGconn* const conn = PQconnectdb(params.build_string_from_options('\'').c_str());

    // Ensure that the connection pointer is freed if we return early.
    std::unique_ptr<PGconn, void(*)(PGconn*)> connPtr(conn, PQfinish);

    if (0 == conn || CONNECTION_OK != PQstatus(conn))
    {
        std::string msg = "Cannot establish connection to the database.";
        if (0 != conn)
        {
            msg += '\n';
            msg += PQerrorMessage(conn);
        }

        throw soci_error(msg);
    }

    if (traceFile_)
    {
        PQtrace(conn, traceFile_);
    }

    if (!timeoutStr.empty())
    {
        set_tcp_user_timeout(PQsocket(conn), timeoutStr);
    }

    // With older PostgreSQL versions we need to change the extra_float_digits
    // parameter to ensure that the conversions to/from text round trip
    // losslessly. This is not necessary since 12.0, which behaves correctly by
    // default, but for older versions use the maximal supported value, which
    // was 2 until 9.x and 3 after it.
    int const version = PQserverVersion(conn);
    if (version < 120000)
    {
        hard_exec(*this, conn,
            version >= 90000 ? "SET extra_float_digits = 3"
                             : "SET extra_float_digits = 2",
            "Cannot set extra_float_digits parameter");
    }

    conn_ = connPtr.release();
    connectionParameters_ = parameters;
}

postgresql_session_backend::~postgresql_session_backend()
{
    clean_up();
}

bool postgresql_session_backend::is_connected()
{
    // For the connection to work, its status must be OK, but this is not
    // sufficient, so try to actually do something with it, even if it's
    // something as trivial as sending an empty command to the server.
    if ( PQstatus(conn_) != CONNECTION_OK )
        return false;

    postgresql_result(*this, PQexec(conn_, "/* ping */"));

    // And then check it again.
    return PQstatus(conn_) == CONNECTION_OK;
}

void postgresql_session_backend::begin()
{
    hard_exec(*this, conn_, "BEGIN", "Cannot begin transaction.");
}

void postgresql_session_backend::commit()
{
    hard_exec(*this, conn_, "COMMIT", "Cannot commit transaction.");
}

void postgresql_session_backend::rollback()
{
    hard_exec(*this, conn_, "ROLLBACK", "Cannot rollback transaction.");
}

void postgresql_session_backend::deallocate_prepared_statement(
    const std::string & statementName)
{
    if (!deallocatePreparedStatements_)
        return;

    const std::string & query = "DEALLOCATE " + statementName;

    hard_exec(*this, conn_, query.c_str(),
        "Cannot deallocate prepared statement.");
}

void postgresql_session_backend::deallocate_all_prepared_statements()
{
    hard_exec(*this, conn_, "DEALLOCATE ALL",
        "Cannot deallocate all prepared statements.");
}

bool postgresql_session_backend::get_next_sequence_value(
    session & s, std::string const & sequence, long long & value)
{
    s << "select nextval('" + sequence + "')", into(value);

    return true;
}

void postgresql_session_backend::clean_up()
{
    if (0 != conn_)
    {
        PQfinish(conn_);
        conn_ = 0;
    }

    if (traceFile_)
    {
        std::fclose(traceFile_);
        traceFile_ = nullptr;
    }
}

std::string postgresql_session_backend::get_next_statement_name()
{
    char nameBuf[20] = { 0 }; // arbitrary length
    snprintf(nameBuf, sizeof(nameBuf), "st_%d", ++statementCount_);
    return nameBuf;
}

postgresql_statement_backend * postgresql_session_backend::make_statement_backend()
{
    return new postgresql_statement_backend(*this, single_row_mode_);
}

postgresql_rowid_backend * postgresql_session_backend::make_rowid_backend()
{
    return new postgresql_rowid_backend(*this);
}

postgresql_blob_backend * postgresql_session_backend::make_blob_backend()
{
    return new postgresql_blob_backend(*this);
}

std::string postgresql_session_backend::get_table_names_query() const
{
    return std::string(R"delim(SELECT table_schema || '.' || table_name AS "TABLE_NAME" FROM information_schema.tables WHERE table_schema in ()delim") + 
                       create_list_of_strings(get_schema_names(const_cast<postgresql_session_backend&>(*this), conn_)) + ")";
}

std::string postgresql_session_backend::get_column_descriptions_query() const
{
    std::vector<std::string> schema_list = get_schema_names(const_cast<postgresql_session_backend&>(*this), conn_);
    return std::string("WITH Schema AS ("
           " SELECT table_schema"
           " FROM information_schema.columns"
           " WHERE table_name = :t"
           " AND CASE"
           " WHEN :s::VARCHAR is not NULL THEN table_schema = :s::VARCHAR"
           " ELSE table_schema in (") + create_list_of_strings(schema_list) + ") END"
           " ORDER BY"
           " CASE table_schema" +
           create_case_list_of_strings(schema_list) +
           " ELSE " + std::to_string(schema_list.size()) + " END"
           " LIMIT 1 )"
           R"( SELECT column_name as "COLUMN_NAME",)"
           R"( data_type as "DATA_TYPE",)"
           R"( character_maximum_length as "CHARACTER_MAXIMUM_LENGTH",)"
           R"( numeric_precision as "NUMERIC_PRECISION",)"
           R"( numeric_scale as "NUMERIC_SCALE",)"
           R"( is_nullable as "IS_NULLABLE")"
           " FROM information_schema.columns"
           " WHERE table_name = :t"
           " AND table_schema = ("
           " SELECT table_schema"
           " FROM Schema )";
}
