#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include "sqlpool.h"

//sql_pool_create(POOL_MAX_NUMBER, "localhost", 3306, "ceshi", "root", "qc123456");  
/*创建连接池*/
SQL_CONN_POOL *sql_pool_create(int connect_pool_number, const char* ip, int port, 
        const char* db_name, const char* user, const char* passwd){
    SQL_CONN_POOL *sp = NULL;
    if (connect_pool_number < 1){
        //printf("connect_pool_number < 1. defalut 1 \n");
        connect_pool_number = 1;
    }
    if ((sp=(SQL_CONN_POOL *)malloc(sizeof(SQL_CONN_POOL))) == NULL){
        //printf("malloc SQL_CONN_POOL error.\n");
        return NULL;
    }

    sp->shutdown    = 0; //开启连接池
    sp->pool_number = 0;
    sp->busy_number = 0;
    strcpy(sp->ip, ip);
    sp->port = port;
    strcpy(sp->db_name, db_name);
    strcpy(sp->user, user);
    strcpy(sp->passwd, passwd);

    /*创建连接*/
    if (connect_pool_number > POOL_MAX_NUMBER)
        connect_pool_number = POOL_MAX_NUMBER;

    for (int index=0; index < connect_pool_number; index++){
        //创建失败
        if (0 != create_db_connect(sp, &sp->sql_pool[index])){
            //销毁连接池
            sql_pool_destroy(sp);
            return NULL;
        }
        //创建成功
        sp->sql_pool[index].index = index;
        sp->pool_number++;
        //printf("create database pool connect:-%d-.\n",sp->sql_pool[index].index); 
    }

    return sp;
}
/*节点创建连接*/
int create_db_connect(SQL_CONN_POOL *sp, SQL_NODE *node){
    int opt=1;
    int res=0; //0正常 -1初始化失败 1 连接失败

    do {
        if (sp->shutdown == 1)
            return -1;
        /*加锁*/
        pthread_mutex_init(&node->lock, NULL);

        /*初始化mysql对象*/
        if (NULL == mysql_init(&node->fd)){
            //printf("mysql init error. \n");
            res = -1;
            break;
        }
        if (!(node->mysql_sock = mysql_real_connect(&node->fd, sp->ip, sp->user, sp->passwd, sp->db_name, sp->port, NULL, 0))){
            //printf("can not connect to mysql.\n");
            node->sql_state = SQL_NODE::DB_DISCONN;
            res = 1;
            break;
        }
        node->used = 0;
        node->sql_state = SQL_NODE::DB_CONN;
        //设置自动连接开启
        mysql_options(&node->fd, MYSQL_OPT_RECONNECT, &opt);
        opt = 3;
        //设置连接超时时间为3s，3s未连接成功则超时
        mysql_options(&node->fd, MYSQL_OPT_CONNECT_TIMEOUT, &opt);
        res = 0;

    }while(0);

    return res;
}
/*销毁连接池*/
void sql_pool_destroy(SQL_CONN_POOL *sp){
    //printf("destroy sql pool ... ... \n");

    sp->shutdown = 1; //关闭连接池
    for (int index=0; index < sp->pool_number; index++){
        if (NULL != sp->sql_pool[index].mysql_sock){
            mysql_close(sp->sql_pool[index].mysql_sock);
            sp->sql_pool[index].mysql_sock = NULL;
        }
        sp->sql_pool[index].sql_state = SQL_NODE::DB_DISCONN; 
        sp->pool_number--;
    }
}
bool PingMysql(SQL_CONN_POOL* sp){
	bool bRet = false;
	SQL_NODE* node=NULL;
	node = get_db_connect(sp);
	if(NULL != node){
		bRet = true;
		release_node(sp,node);
	}
	return bRet;
}
/*取出一个未使用的连接*/
SQL_NODE *get_db_connect(SQL_CONN_POOL *sp){
    //获取一个未使用的连接，用随机值访问index，保证每次访问每个节点的概率基本相同
    int start_index = 0, index = 0, i;
    int ping_res;

    if (sp->shutdown == 1)
        return NULL;

    srand((int)time(0)); //根据当前时间生成随机数
    start_index = rand() % sp->pool_number; //访问的开始地址

    for (i=0; i < sp->pool_number; i++){
        index = (start_index + i) % sp->pool_number;

        if (!pthread_mutex_trylock(&sp->sql_pool[index].lock)){
            if (SQL_NODE::DB_DISCONN == sp->sql_pool[index].sql_state){
                //重新连接
                if (0 != create_db_connect(sp, &(sp->sql_pool[index]))){
                    //重新连接失败
                    release_node(sp, &(sp->sql_pool[index]));
                    continue;
                }
            }
            //检查服务器是否关闭了连接
            ping_res = mysql_ping(sp->sql_pool[index].mysql_sock);
            if (0 != ping_res){
                //printf("mysql ping error.\n");
                sp->sql_pool[index].sql_state = SQL_NODE::DB_DISCONN;
                release_node(sp, &(sp->sql_pool[index]));
            } else {
                sp->sql_pool[index].used = 1;
                sp->busy_number++;//被获取的数量增1
                break ;  //只需要一个节点
            }
        }
    }

    if (i == sp->pool_number)
        return NULL;
    else
        return &(sp->sql_pool[index]);
}
/*归回连接*/
void release_node(SQL_CONN_POOL *sp, SQL_NODE *node){
    node->used = 0;
    sp->busy_number--;
    pthread_mutex_unlock(&node->lock);
}
/*增加或删除连接*/
SQL_CONN_POOL *changeNodeNum(SQL_CONN_POOL *sp, int op) { //增加或减少5个连接
    int Num = 5;
    int index;
    int endindex;

    if (op == 1) { //增加    0减少
        endindex = sp->pool_number + Num;

        /*创建连接*/
        for (index=sp->pool_number; index < endindex; index++) {
            //创建失败
            if (0 != create_db_connect(sp, &sp->sql_pool[index])){
                //销毁连接池
                sql_pool_destroy(sp);
                return NULL;
            }
            //创建成功
            sp->sql_pool[index].index = index;                                                           
            sp->pool_number++;
            //printf("create database pool connect:-%d-.\n",sp->sql_pool[index].index); 
        }
    } else if (op == 0) {
        endindex = sp->pool_number - Num -1;
        //减少连接
        for (index=sp->pool_number-1; index>endindex && index>=0; index--) { 
            if (NULL != sp->sql_pool[index].mysql_sock) {
                mysql_close(sp->sql_pool[index].mysql_sock);
                sp->sql_pool[index].mysql_sock = NULL;
            }
            sp->sql_pool[index].sql_state = SQL_NODE::DB_DISCONN; 
            sp->pool_number--;
            //printf("delete database pool connect:-%d-.\n",sp->sql_pool[index].index);
        }
    }
    return sp;
}
/*执行无响应的SQL语句*/
bool execute_sql(SQL_NODE* node,const char* sql){
	int res = mysql_real_query(node->mysql_sock,sql,strlen(sql));
	return (res == 0);
}
/*
bool execute_sql_procedure(SQL_NODE* node,const char* sql,
				std::vector<std::vector<std::string> >& vResult	){
	if(mysql_query(node->mysql_sock,sql)){
		return false;
	}
	mysql_query(node->mysql_sock,"select @t");
	MYSQL_RES* res = mysql_store_result(node->mysql_sock);
	if(NULL == res){
		mysql_free_result(res);
		return false;
	}
	int numcols = mysql_num_fields(res);
	MYSQL_ROW row;
	while(row = mysql_fetch_row(res)){
		std::vector<std::string> row_vec;
		row_vec.reserve(numcols);
		for(int nIndex = 0; nIndex < numcols;nIndex++){
			printf("%s ",row[nIndex]);
			//row_vec.push_back(row[nIndex]);
		}
		printf("\n");
		//vResult.push_back(row_vec);
	}
	mysql_free_result(res);
	return true;
}*/

/*执行有返回的SQL语句*/
bool execute_sql_rtn(SQL_NODE * node,const char * sql,
				std::vector<std::vector<std::string> >& vResult){
	if(!execute_sql(node,sql)){
		return false;
	}	
	MYSQL_RES* res = mysql_store_result(node->mysql_sock);
	if(NULL == res){
		mysql_free_result(res);
		return false;
	}
	int numcols = mysql_num_fields(res);
	MYSQL_ROW row;
	while(row = mysql_fetch_row(res)){
		std::vector<std::string> row_vec;
		row_vec.reserve(numcols);
		for(int nIndex = 0; nIndex < numcols;nIndex++){
			if(NULL == row[nIndex])
				row_vec.push_back("");
			else
				row_vec.push_back(row[nIndex]);
		}
		vResult.push_back(row_vec);
	}
	mysql_free_result(res);
	return true;
}
/*执行出错信息获取*/
std::string get_execute_errmsg(SQL_NODE* node){
	char buf[2048] = {0};
	snprintf(buf,2047,"error_code=%d,error_msg=%s",mysql_errno(node->mysql_sock),mysql_error(node->mysql_sock));
	return buf;
}
/*
int main(){
	SQL_CONN_POOL *sp = 
        sql_pool_create(10, "localhost", 3306, "test", "root", "123456");  
    SQL_NODE *node  = get_db_connect(sp);
	if (NULL == node) {  
        printf("get sql pool node error.\n");  
        return -1;  
    } 
	//const char* sql = "call select_c1(1,@t);";
	char buff[1024] = {0};
    strcpy(buff,"call SelectAll('c1')");
    //sprintf(buff,"call cal_grade(%d,%d,@t,%f,%s,%s)",10,10,0.3,"123","456");
    std::vector<std::vector<std::string> > vResult;
	//if(!execute_sql_procedure(node,buff,vResult)){
	if(!execute_sql_rtn(node,buff,vResult)){
    	printf("error:%s\n",get_execute_errmsg(node).c_str());
		return -1;
	}
	for(int i=0; i< vResult.size();i++){
		for(int j=0; j< vResult[i].size();j++){
			printf("%s ",vResult[i][j].c_str());
		}
		printf("\n");
	}
	sql_pool_destroy(sp);
	return 0;
}
int main1()  
{  
    //MYSQL_FIELD *fd;  
    SQL_CONN_POOL *sp = 
        sql_pool_create(10, "localhost", 3306, "test", "root", "123456");  
    SQL_NODE *node  = get_db_connect(sp);  
    SQL_NODE *node2 = get_db_connect(sp);

    if (NULL == node) {  
        printf("get sql pool node error.\n");  
        return -1;  
    } 
    printf("--%d-- \n", node->index);
    printf("busy--%d--\n", sp->busy_number);

    if (mysql_query(&(node->fd), "select * from c1"))
    {  												     
        printf("query error.\n");  								  
        return -1;  									
    }
    else  
    {  
        printf("succeed!\n");  
    }
    changeNodeNum(sp, 0);//减少
    changeNodeNum(sp, 1);//增加
    sql_pool_destroy(sp);
    return 0;  
} */
