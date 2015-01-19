#if !defined(__SMTC_CLI_H__)
#define __SMTC_CLI_H__

#include "mem_pool.h"
#include "shm_queue.h"
#include "smtc_priv.h"
#include "smtc_ssvr.h"

/* 发送服务命令套接字 */
#define smtc_ssvr_usck_path(conf, path, tidx) \
    snprintf(path, sizeof(path), "./tmp/smtc/snd/%s/usck/%s_ssvr_%d.usck", \
        conf->name, conf->name, tidx+1)

/* 发送对象信息 */
typedef struct
{
    smtc_ssvr_conf_t conf;          /* 客户端配置信息 */
    log_cycle_t *log;               /* 日志对象 **/
    mem_pool_t *pool;               /* 内存池 */
    
    int cmdfd;                      /* 命令套接字 */
    shm_queue_t **sq;               /* 发送缓冲队列 */
} smtc_cli_t;

extern smtc_cli_t *smtc_cli_init(const smtc_ssvr_conf_t *conf, int idx, log_cycle_t *log);
extern int smtc_cli_send(smtc_cli_t *cli, int type, const void *data, size_t size);

#endif /*__SMTC_SND_CLI_H__*/