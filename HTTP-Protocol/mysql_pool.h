#include<queue>
#include<iostream>
#include<mysql/mysql.h>
#include<mutex>

class MySQLPool {
private:
    std::queue<MYSQL*> pool; //存放MySQL连接的队列
    std::mutex mtx;
    int max_conn;
    static MySQLPool* instance;

    MySQLPool(){
        max_conn = 10; //默认创建最多10个连接
        init();
    }

    MYSQL* create_conn(){
        MYSQL* conn = mysql_init(NULL); //空的MySQL连接对象
        //将其连接上3306端口(MySQL固定端口)，本地的webserver数据库，账号密码为webserver/123456
        if(!mysql_real_connect(conn,"localhost","root","123456","webserver",3306,NULL,0)){
            std::cout << "MySQL连接失败: " << mysql_error(conn) << std::endl; 
            return NULL;
        }
        return conn;
    }

    void init(){
        for(int i = 0; i < max_conn; i++){
            MYSQL* conn = create_conn();
            //判断创建成功，丢进任务队列！
            if(conn){
                pool.push(conn);
            }
        }
    }

public:
    //重点：单例模式，创建MySQLPool的函数为private，只能在尚不存在连接池的时候通过get_instance获取一个实例，之后再调动不会再创建新对象
    //创建后instance指向这个唯一实例，这样的操作禁止了私自创建连接池，保证只存在一个连接池
    static MySQLPool* get_instance(){
        if(!instance) instance = new MySQLPool();
        return instance;
    }
    //线程用来从连接池里申请连接的函数
    MYSQL* get_conn(){
        //由于要对连接队列进行操作，需要加锁，防止多个线程同时申请一个MYSQL连接
        std::lock_guard<std::mutex> lock(mtx);
        if(pool.empty()){
            return NULL;
        }
        MYSQL* conn = pool.front();
        pool.pop();
        return conn;
    }
    //连接用完了要还给连接池
    void return_conn(MYSQL* conn){
        std::lock_guard<std::mutex>lock(mtx);
        pool.push(conn);
    }
};
//要类外初始化！！！
MySQLPool* MySQLPool::instance = NULL;