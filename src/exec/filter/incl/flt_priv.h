#if !defined(__FLT_PRIV_H__)
#define __FLT_PRIV_H__

#include "log.h"
#include "redis.h"
#include "sck_api.h"

/* 宏定义 */
#define FLT_THD_DEF_NUM             (05)    /* 默认线程数 */
#define FLT_THD_MIN_NUM             (01)    /* 最小线程数 */
#define FLT_SLAB_SIZE               (50 * MB)   /* SLAB内存池大小 */
#define FLT_DOMAIN_IP_MAP_HASH_MOD  (1777)  /* 域名IP映射表模 */
#define FLT_DOMAIN_BLACKLIST_HASH_MOD (10)  /* 域名黑名单表长度 */
#define FLT_WORKQ_MAX_NUM           (2000)  /* 工作队列单元数 */
#define FLT_TASK_STR_LEN            (8192)  /* TASK字串最大长度 */

#define FLT_DEF_CONF_PATH           "../conf/filter.xml"   /* 默认配置路径 */

#define flt_get_task_str(str, size, uri, deep, ip, family) /* TASK字串格式 */\
    snprintf(str, size, \
        "<TASK>" \
            "<TYPE>%d</TYPE>"   /* Task类型 */\
            "<BODY>" \
                "<IP FAMILY='%d'>%s</IP>"  /* IP地址和协议 */\
                "<URI DEPTH='%d'>%s</URI>" /* 网页深度&URI */\
            "</BODY>" \
        "</TASK>", 1, family, ip, deep, uri);

/* 错误码 */
typedef enum
{
    FLT_OK = 0                             /* 正常 */
    , FLT_SHOW_HELP                        /* 显示帮助信息 */

    , FLT_ERR = ~0x7FFFFFFF                /* 失败、错误 */
} flt_err_code_e;

/* 数据类型 */
typedef enum
{
    FLT_DATA_TYPE_UNKNOWN = 0              /* 未知数据 */
    , FLT_HTTP_GET_REQ                     /* HTTP GET请求 */
} flt_data_type_e;

/* 输入参数信息 */
typedef struct
{
    char conf_path[FILE_NAME_MAX_LEN];      /* 配置文件路径 */
    bool isdaemon;                          /* 是否后台运行 */
} flt_opt_t;

/* 网页基本信息 */
typedef struct
{
    char fname[FILE_NAME_MAX_LEN];  /* 网页信息文件 后缀:.wpi */

    char uri[URI_MAX_LEN];          /* URI */
    uint32_t depth;                 /* 网页深度 */
    char ip[IP_ADDR_MAX_LEN];       /* IP地址 */
    int port;                       /* 端口号 */
    char html[FILE_NAME_MAX_LEN];   /* 网页存储名 */
    int size;                       /* 网页大小 */
} flt_webpage_info_t;

/* 域名IP映射信息 */
typedef struct
{
    char host[HOST_MAX_LEN];        /* Host信息(域名) */

    int ip_num;                     /* IP地址数 */
#define FLT_IP_MAX_NUM  (8)
    ipaddr_t ip[FLT_IP_MAX_NUM];    /* 域名对应的IP地址 */
    time_t access_tm;               /* 最近访问时间 */
} flt_domain_ip_map_t;

/* 域名黑名单信息 */
typedef struct
{
    char host[HOST_MAX_LEN];        /* Host信息(域名) */

    time_t access_tm;               /* 最近访问时间 */
} flt_domain_blacklist_t;

/* 内部接口 */
int flt_getopt(int argc, char **argv, flt_opt_t *opt);
int flt_usage(const char *exec);
int flt_proc_lock(void);
void flt_set_signal(void);

int flt_domain_ip_map_cmp_cb(const char *domain, const flt_domain_ip_map_t *map);
int flt_domain_blacklist_cmp_cb(const char *domain, const flt_domain_blacklist_t *blacklist);

bool flt_set_uri_exists(redis_clst_t *ctx, const char *hash, const char *uri);
/* 判断uri是否已下载 */
#define flt_is_uri_down(cluster, hash, uri) flt_set_uri_exists(cluster, hash, uri)
/* 判断uri是否已推送 */
#define flt_is_uri_push(cluster, hash, uri) flt_set_uri_exists(cluster, hash, uri)

#endif /*__FLT_PRIV_H__*/
