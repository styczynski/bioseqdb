#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <libpq-fe.h>

void check_pg(PGconn* connection, PGresult* result) {
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        std::cerr << "\x1B[1;31merror:\x1B[0m postgres error\n\x1B[1;33mdetails:\x1B[0m\n" << PQerrorMessage(connection);
        PQclear(result);
        std::exit(1);
    }
    PQclear(result);
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "\x1B[1;31merror:\x1B[0m invalid command-line arguments\n\x1B[1;34musage:\x1B[0m " << argv[0] << " <TABLE> <NAME COLUMN> <SEQUENCE COLUMN> <FASTA FILE>\n";
        return 1;
    }

    if (std::getenv("DB_URI") == nullptr) {
        std::cerr << "\x1B[1;31merror:\x1B[0m unset DB_URI environment variable\n\x1B[1;34musage:\x1B[0m\n";
        return 1;
    }

    std::string_view table = argv[1];
    std::string_view column_name = argv[2];
    std::string_view column_sequence = argv[3];
    std::string_view fasta_file_path = argv[4];
    std::string_view postgres_url = std::getenv("DB_URI");

    std::cout << "opening fasta file" << std::endl;
    std::ifstream fasta_file(fasta_file_path.data());

    std::cout << "connecting to postgres instance" << std::endl;
    PGconn* connection = PQconnectdb(postgres_url.data());
    if (PQstatus(connection) != CONNECTION_OK) {
        std::cerr << "\x1B[1;31merror:\x1B[0m postgres error \x1B[1;33mcaused by\x1B[0m " << PQerrorMessage(connection) << "\n";
        PQfinish(connection);
        return 1;
    }

    check_pg(connection, PQexec(connection, "BEGIN;"));

    std::string current_name;
    std::string current_sequence;
    std::string current_line;
    std::string insert_query = std::string("INSERT INTO ") + table.data() + "(" + column_name.data() + ", " + column_sequence.data() + ") VALUES ($1, $2);";
    auto try_submit_sequence = [&]{
        if (current_sequence.length() > 0) {
            std::cout << "inserting sequence '" << current_name << "' [" << current_sequence.length() << " bytes]\n";
            const char* params[2] = {current_name.c_str(), current_sequence.c_str()};
            check_pg(connection, PQexecParams(connection, insert_query.c_str(), 2, nullptr, params, nullptr, nullptr, 1));
            current_sequence.clear();
        }
        current_name.clear();
    };
    while (std::getline(fasta_file, current_line)) {
        if (!current_line.empty() && current_line[0] == '>') {
            try_submit_sequence();
            current_name = current_line.substr(1);
        } else {
            for (char& chr : current_line)
                chr = (char) std::toupper(chr);
            current_sequence += current_line;
        }
    }
    try_submit_sequence();

    check_pg(connection, PQexec(connection, "COMMIT;"));

    PQfinish(connection);
}
