#include <memory.h>
#include <assert.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "smtp.h"
#include "smtp_cmd.h"
#include "smtp_comm.h"
#include "smtp_recv.h"
#include "thread_pool.h"

/* 静态函数 */
static int smtp_listen_init();
static int smtp_lsn_accept(smtp_cntx_t *ctx, smtp_listen_t *lsn);

static int smtp_lsn_cmd_core_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn);
static int smtp_cmd_to_recvtp(smtp_cntx_t *ctx, int cmd_sck_id, const smtp_cmd_t *cmd);
static int smtp_lsn_cmd_query_conf_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn, smtp_cmd_t *cmd);
static int smtp_lsn_cmd_query_recv_stat_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn, smtp_cmd_t *cmd);
static int smtp_lsn_cmd_query_work_stat_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn, smtp_cmd_t *cmd);

/* 随机选择接收线程 */
#define smtp_rand_recv(ctx) ((ctx)->listen.total++ % (ctx->recvtp->num))

/******************************************************************************
 **函数名称: smtp_listen_routine
 **功    能: 启动SMTP侦听线程
 **输入参数: 
 **     conf: 配置信息
 **     log: 日志对象
 **输出参数: NONE
 **返    回: 全局对象
 **实现描述: 
 **     1. 初始化侦听
 **     2. 等待请求和命令
 **     3. 接收请求和命令
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
void *smtp_listen_routine(void *args)
{
    fd_set rdset;
    int ret, max;
    smtp_listen_t *lsn;
    struct timeval timeout;
    char path[FILE_PATH_MAX_LEN];
    smtp_conf_t *conf = &ctx->conf;
    smtp_cntx_t *ctx = (smtp_cntx_t *)args;

    /* 1. 初始化侦听 */
    lsn = smtp_listen_init(ctx)
    if (NULL == lsn)
    {
        log_error(ctx->log, "Initialize listen failed!");
        abort();
        return (void *)-1;
    }

    for (;;)
    {
        /* 2. 等待请求和命令 */
        FD_ZERO(&rdset);

        FD_SET(lsn->lsn_sck_id, &rdset);
        FD_SET(lsn->cmd_sck_id, &rdset);

        max = (lsn->lsn_sck_id > lsn->cmd_sck_id)? lsn->lsn_sck_id : lsn->cmd_sck_id;

        timeout.tv_sec = SMTP_LSN_TMOUT_SEC;
        timeout.tv_usec = SMTP_LSN_TMOUT_USEC;
        ret = select(max+1, &rdset, NULL, NULL, &timeout);
        if (ret < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }

            log_error(lsn->log, "errmsg:[%d] %s", errno, strerror(errno));
            abort();
            return (void *)-1;
        }
        else if (0 == ret)
        {
            continue;
        }

        /* 3. 接收连接请求 */
        if (FD_ISSET(lsn->lsn_sck_id, &rdset))
        {
            smtp_lsn_accept(ctx, lsn);
        }

        /* 4. 接收处理命令 */
        if (FD_ISSET(lsn->cmd_sck_id, &rdset))
        {
            smtp_lsn_cmd_core_hdl(ctx, lsn);
        }
    }

    pthread_exit(NULL);
    return (void *)-1;
}

/******************************************************************************
 ** Name : smtp_lsn_destroy
 ** Desc : Destroy listen-thread
 ** Input: 
 **     lsn: Accept thread
 ** Output: NONE
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 **     1. Close file descriptor
 **     2. Kill thread
 ** Note : 
 ** Author: # Qifeng.zou # 2014.04.15 #
 ******************************************************************************/
int smtp_lsn_destroy(smtp_listen_t *lsn)
{
    Close(lsn->lsn_sck_id);
    Close(lsn->cmd_sck_id);

    pthread_cancel(lsn->tid);
    return SMTP_OK;
}

/******************************************************************************
 **函数名称: smtp_listen_init
 **功    能: 启动SMTP侦听线程
 **输入参数: 
 **     conf: 配置信息
 **输出参数: NONE
 **返    回: 侦听对象
 **实现描述: 
 **     1. 侦听指定端口
 **     2. 创建CMD套接字
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static smtp_listen_t *smtp_listen_init(smtp_cntx_t *ctx)
{
    smtp_listen_t *lsn = &ctx->listen;

    lsn->log = ctx->log;

    /* 1. 侦听指定端口 */
    lsn->lsn_sck_id = Listen(ctx->conf.port);
    if (lsn->lsn_sck_id < 0)
    {
        log_error(lsn->log, "Listen special port failed!");
        return NULL;
    }

    /* 2. 创建CMD套接字 */
    smtp_lsn_usck_path(conf, path);

    lsn->cmd_sck_id = usck_udp_creat(path);
    if (lsn->cmd_sck_id < 0)
    {
        Close(lsn->lsn_sck_id);
        log_error(lsn->log, "Create unix udp socket failed!");
        return NULL;
    }

    return lsn;
}

/******************************************************************************
 **函数名称: smtp_listen_accept
 **功    能: 接收连接请求
 **输入参数: 
 **     ctx: 全局对象
 **     lsn: 侦听对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **     1. 接收连接请求
 **     2. 发送至接收端
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static int smtp_lsn_accept(smtp_cntx_t *ctx, smtp_listen_t *lsn)
{
    int sckid;
    socklen_t len;
    smtp_cmd_t cmd;
    struct sockaddr_in cliaddr;
    smtp_cmd_add_sck_t *args = (smtp_cmd_add_sck_t *)&cmd.args;

    /* 1. 接收连接请求 */
    for (;;)
    {
        memset(&cliaddr, 0, sizeof(cliaddr));

        len = sizeof(struct sockaddr_in);
        
        sckid = accept(lsn->lsn_sck_id, (struct sockaddr *)&cliaddr, &len);
        if (sckid >= 0)
        {
            log_debug(lsn->log, "New connection! sckid:%d ip:%s",
                    sckid, inet_ntoa(cliaddr.sin_addr));
            break;
        }
        else if (EINTR == errno)
        {
            continue;
        }

        log_error(lsn->log, "errmsg:[%d] %s", errno, strerror(errno));
        return SMTP_ERR;
    }

    fd_set_nonblocking(sckid);

    /* 2. 发送至接收端 */
    memset(&cmd, 0, sizeof(cmd));

    cmd.type = SMTP_CMD_ADD_SCK;
    args->sckid = sckid; 
    snprintf(args->ipaddr, sizeof(args->ipaddr), "%s", inet_ntoa(cliaddr.sin_addr));

    if (smtp_cmd_to_recvtp(ctx, lsn->cmd_sck_id, &cmd) < 0)
    {
        Close(sckid);
        log_error(lsn->log, "Send command failed! sckid:[%d]", sckid);
        return SMTP_ERR;
    }

    return SMTP_OK;
}

/******************************************************************************
 **函数名称: smtp_lsn_cmd_core_hdl
 **功    能: 接收和处理命令
 **输入参数: 
 **     ctx: 全局对象
 **     lsn: 侦听对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **     1. 接收命令
 **     2. 处理命令
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static int smtp_lsn_cmd_core_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn)
{
    smtp_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));

    /* 1. 接收命令 */
    if (usck_udp_recv(lsn->cmd_sck_id, (void *)&cmd, sizeof(cmd)) < 0)
    {
        log_error(lsn->log, "Recv command failed! errmsg:[%d] %s", errno, strerror(errno));
        return SMTP_ERR_RECV_CMD;
    }

    /* 2. 处理命令 */
    switch (cmd.type)
    {
        case SMTP_CMD_QUERY_CONF_REQ:
        {
            return smtp_lsn_cmd_query_conf_hdl(ctx, lsn, &cmd);
        }
        case SMTP_CMD_QUERY_RECV_STAT_REQ:
        {
            return smtp_lsn_cmd_query_recv_stat_hdl(ctx, lsn, &cmd);
        }
        case SMTP_CMD_QUERY_WORK_STAT_REQ:
        {
            return smtp_lsn_cmd_query_work_stat_hdl(ctx, lsn, &cmd);
        }
        default:
        {
            log_error(lsn->log, "Unknown command! type:[%d]", cmd.type);
            return SMTP_ERR_UNKNOWN_CMD;
        }
    }

    return SMTP_ERR_UNKNOWN_CMD;
}

/******************************************************************************
 ** Name : smtp_cmd_to_recvtp
 ** Desc : Send command to one thread of recv-thread-pool
 ** Input: 
 **     ctx: Global context
 **     cmd_sck_id: Command FD 
 **     cmd: Command
 ** Output: 
 **     fail_cmdq: Fail queue - Store fail command into this queue. 
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 **     1. Random select recv-thread
 **     2. Get recv-thread path
 **     3. Send command
 ** Note : 
 **     Resend command SMTP_RECV_CMD_RESND_TIMES when send failed.
 ** Author: # Qifeng.zou # 2014.05.09 #
 ******************************************************************************/
static int smtp_cmd_to_recvtp(smtp_cntx_t *ctx, int cmd_sck_id, const smtp_cmd_t *cmd)
{
    int ret = 0, tidx = 0, times = 0;
    char path[FILE_PATH_MAX_LEN];
    smtp_conf_t *conf = &ctx->conf;

AGAIN:
    memset(path, 0, sizeof(path));

    /* 1. Select recv-thread */
    tidx = smtp_rand_recv(ctx);

    /* 2. Get recv-thread path */
    smtp_rsvr_usck_path(conf, path, tidx);

    /* 3. Send command */
    ret = usck_udp_send(cmd_sck_id, path, cmd, sizeof(smtp_cmd_t));
    if (ret < 0)
    {
        if (times++ < SMTP_RECV_CMD_RESND_TIMES)
        {
            goto AGAIN;
        }
        
        log_error(lsn->log, "Send cmd failed! path:[%d] type:[%d]", path, cmd->type);
        return SMTP_ERR;
    }

    return SMTP_OK;
}

/******************************************************************************
 **函数名称: smtp_lsn_cmd_query_conf_hdl
 **功    能: 查询配置信息
 **输入参数: 
 **     ctx: 全局对象
 **     lsn: 侦听对象
 **     cmd: 处理命令
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **     1. 设置应答参数
 **     2. 发送应答信息
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static int smtp_lsn_cmd_query_conf_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn, smtp_cmd_t *cmd)
{
    int ret;
    smtp_cmd_t rep;
    smtp_conf_t *cf = &ctx->conf;
    smtp_cmd_conf_t *args = (smtp_cmd_conf_t *)&rep.args;

    memset(&rep, 0, sizeof(rep));

    /* 1. 设置应答信息 */
    rep.type = SMTP_CMD_QUERY_CONF_REP;

    snprintf(args->name, sizeof(args->name), "%s", cf->name);
    args->port = cf->port;
    args->recv_thd_num = cf->recv_thd_num;
    args->work_thd_num = cf->wrk_thd_num;
    args->recvq_num = cf->recvq_num;

    args->qmax = cf->recvq.max;
    args->qsize = cf->recvq.size;

    /* 2. 发送应答信息 */
    if (usck_udp_send(lsn->cmd_sck_id, cmd->src_path, &rep, sizeof(rep)) < 0)
    {
        log_error(lsn->log, "errmsg:[%d] %s!", errno, strerror(errno));
        return SMTP_ERR;
    }

    return SMTP_OK;
}

/******************************************************************************
 **函数名称: smtp_lsn_cmd_query_recv_stat_hdl
 **功    能: 查询接收线程状态
 **输入参数: 
 **     ctx: 全局对象
 **     lsn: 侦听对象
 **     cmd: 处理命令
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **     1. 设置应答信息
 **     2. 发送应答信息
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static int smtp_lsn_cmd_query_recv_stat_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn, smtp_cmd_t *cmd)
{
    int idx;
    smtp_cmd_t rep;
    smtp_cmd_recv_stat_t *stat = (smtp_cmd_recv_stat_t *)&rep.args;
    const smtp_rsvr_t *recv = (const smtp_rsvr_t *)ctx->recvtp->data;

    for (idx=0; idx<ctx->conf.recv_thd_num; ++idx, ++recv)
    {
        /* 1. 设置应答信息 */
        rep.type = SMTP_CMD_QUERY_RECV_STAT_REP;

        stat->connections = recv->connections;
        stat->recv_num = recv->recv_num;
        stat->drop_total = recv->drop_total;
        stat->err_total = recv->err_total;

        /* 2. 发送命令信息 */
        if (usck_udp_send(rsvr->cmd_sck_id, cmd->src_path, &rep, sizeof(rep)) < 0)
        {
            log_error(lsn->log, "errmsg:[%d] %s!", errno, strerror(errno));
            return SMTP_ERR;
        }
    }
    return SMTP_OK;
}

/******************************************************************************
 **函数名称: smtp_lsn_cmd_query_work_stat_hdl
 **功    能: 查询工作线程状态
 **输入参数: 
 **     ctx: 全局对象
 **     lsn: 侦听对象
 **     cmd: 处理命令
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **     1. 设置应答信息
 **     2. 发送应答信息
 **注意事项: 
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static int smtp_lsn_cmd_query_work_stat_hdl(smtp_cntx_t *ctx, smtp_listen_t *lsn, smtp_cmd_t *cmd)
{
    int idx;
    smtp_cmd_t rep;
    smtp_cmd_work_stat_t *stat = (smtp_cmd_work_stat_t *)&rep.args;
    const smtp_worker_t *worker = (smtp_worker_t *)ctx->worktp->data;

    for (idx=0; idx<ctx->conf.wrk_thd_num; ++idx, ++worker)
    {
        /* 1. 设置应答信息 */
        rep.type = SMTP_CMD_QUERY_WORK_STAT_REP;

        stat->work_total = worker->work_total;
        stat->drop_total = worker->drop_total;
        stat->err_total = worker->err_total;

        /* 2. 发送应答信息 */
        if (usck_udp_send(worker->cmd_sck_id, cmd->src_path, &rep, sizeof(rep)) < 0)
        {
            log_error(lsn->log, "errmsg:[%d] %s!", errno, strerror(errno));
            return SMTP_ERR;
        }
    }

    return SMTP_OK;
}
