#include "database.h"
#include <stdexcept>
#include <cstring>

Database::Database(const std::string& host,
                   const std::string& user,
                   const std::string& password,
                   const std::string& dbname,
                   unsigned int port)
{
    conn_ = mysql_init(nullptr);
    if (!conn_)
        throw std::runtime_error("mysql_init failed");

    if (!mysql_real_connect(conn_,
                            host.c_str(),
                            user.c_str(),
                            password.c_str(),
                            dbname.c_str(),
                            port, nullptr, 0))
    {
        std::string err = mysql_error(conn_);
        mysql_close(conn_);
        throw std::runtime_error("mysql_real_connect failed: " + err);
    }

    // 设置字符集为 utf8
    mysql_set_character_set(conn_, "utf8mb4");
}

Database::~Database() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

Result Database::query(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str()))
        throw std::runtime_error("query failed: " + std::string(mysql_error(conn_)));

    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        if (mysql_field_count(conn_) == 0) return {}; // 非 SELECT
        throw std::runtime_error("mysql_store_result failed: " +
                                 std::string(mysql_error(conn_)));
    }

    Result rows;
    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        Row r;
        unsigned long* lengths = mysql_fetch_lengths(res);
        for (int i = 0; i < num_fields; i++) {
            r[fields[i].name] = row[i] ? std::string(row[i], lengths[i]) : "";
        }
        rows.push_back(std::move(r));
    }

    mysql_free_result(res);
    return rows;
}

int Database::execute(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str()))
        throw std::runtime_error("execute failed: " + std::string(mysql_error(conn_)));
    return static_cast<int>(mysql_affected_rows(conn_));
}

std::string Database::escape(const std::string& s) {
    std::string buf(s.size() * 2 + 1, '\0');
    unsigned long len = mysql_real_escape_string(conn_, &buf[0],
                                                  s.c_str(), s.size());
    buf.resize(len);
    return buf;
}

uint64_t Database::last_insert_id() {
    return mysql_insert_id(conn_);
}