#ifndef DATABASE_H
#define DATABASE_H

#include <mariadb/mysql.h>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdint>

using Row    = std::map<std::string, std::string>;
using Result = std::vector<Row>;

class Database {
public:
    Database(const std::string& host,
             const std::string& user,
             const std::string& password,
             const std::string& dbname,
             unsigned int port = 3306);
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    Result      query(const std::string& sql);
    int         execute(const std::string& sql);
    std::string escape(const std::string& s);
    uint64_t    last_insert_id();

private:
    MYSQL* conn_;
};

#endif // DATABASE_H