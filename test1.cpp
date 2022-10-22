#include <string.h>
#include <stdio.h>
#include <mysql/mysql.h>
#include <iostream>
#include <unistd.h>

using namespace std;

int main() {
    MYSQL  * con = nullptr;
    con = mysql_init(con);
    cout << "即将连接" << endl;
    if(mysql_real_connect(con, "localhost", "root", "000", "zhou", 3306, NULL, 0)) {
        cout << "数据库连接成功" << endl;
    }
    else {
        cout << "数据库连接失败" << endl;
        return 0;
    }

    int ret = mysql_query(con, "SELECT username,passwd FROM user");
    
    cout << mysql_error(con) << endl;
    if(ret != 0) cout << "error" << endl;
    else cout << "查询到结果" << endl;
    //sleep(10000);
    mysql_close(con);
}