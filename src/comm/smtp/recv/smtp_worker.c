#include <memory.h>
#include <assert.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "smtp.h"
#include "xml_tree.h"
#include "smtp_cmd.h"
#include "smtp_comm.h"
#include "smtp_recv.h"
#include "thread_pool.h"


/* 静态函数 */
static smtp_worker_t *smtp_worker_get_curr(smtp_cntx_t *ctx);
static int smtp_worker_init(smtp_cntx_t *ctx, smtp_worker_t *worker);
static int smtp_worker_core_handler(smtp_cntx_t *ctx, smtp_worker_t *worker);
static int smtp_worker_cmd_hdl(smtp_cntx_t *ctx, smtp_worker_t *worker, const smtp_cmd_t *cmd);

/******************************************************************************
 ** Name : smtp_creat_worktp
 ** Desc : 创建Work线程池
 ** Input: 
 **     conf: 配置参数
 ** Output: 
 **     recvtp: Recv Thread-Pool
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.04.11 #
 ******************************************************************************/
int smtp_creat_worktp(smtp_cntx_t *ctx)
{
    int ret = 0, idx = 0;
    smtp_worker_t *worker = NULL;
    smtp_conf_t *conf = &ctx->conf;

    ret = thread_pool_init(&ctx->worktp, conf->wrk_thd_num);
    if (SMTP_OK != ret)
    {
        LogError("Initialize thread pool failed!");
        return SMTP_ERR;
    }

    ctx->worktp->data = (void *)calloc(conf->wrk_thd_num, sizeof(smtp_worker_t));
    if (NULL == ctx->worktp->data)
    {
        LogError("errmsg:[%d] %s!", errno, strerror(errno));
        thread_pool_destroy(ctx->worktp);
        ctx->worktp = NULL;
        return SMTP_ERR;
    }

    worker = ctx->worktp->data;
    for (idx=0; idx<conf->wrk_thd_num; ++idx, ++worker)
    {
        worker->tidx = idx;
        smtp_worker_init(ctx, worker);
    }
    
    return SMTP_OK;
}

/******************************************************************************
 ** Name : smtp_worktp_destroy
 ** Desc : Destroy work-thread-pool
 ** Input: 
 ** Output: 
 **     _ctx: Global context
 **     args: worktp->data
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.04.15 #
 ******************************************************************************/
void smtp_worktp_destroy(void *_ctx, void *args)
{
    int idx = 0, n = 0;
    smtp_cntx_t *ctx = (smtp_cntx_t *)_ctx;
    smtp_worker_t *worker = (smtp_worker_t *)args;
    smtp_conf_t *conf = &ctx->conf;

    for (idx=0; idx<conf->wrk_thd_num; ++idx, ++worker)
    {
        /* 1. Close command fd */
        Close(worker->cmd_sck_id);
    }

    Free(args);

    return;
}

/******************************************************************************
 ** Name : smtp_worker_routine
 ** Desc : Start work handler
 ** Input: 
 **     args: Global context
 ** Output: NONE
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 **     1. Init worker context
 **     2. Wait event
 **     3. Call work handler
 ** Note : 
 ** Author: # Qifeng.zou # 2014.04.10 #
 ******************************************************************************/
void *smtp_worker_routine(void *args)
{
    int max = 0, ret = 0, idx;
    struct timeval timeout;
    smtp_cntx_t *ctx = (smtp_cntx_t *)args;
    smtp_worker_t *worker = NULL;

    /* 1. Get current worker context */
    worker = smtp_worker_get_curr(ctx);
    if (NULL == worker)
    {
        LogError("Initialize rsvr failed!");
        pthread_exit(NULL);
        return (void *)-1;
    }

    for (;;)
    {
        /* 2. Wait event */
        FD_ZERO(&worker->rdset);

        FD_SET(worker->cmd_sck_id, &worker->rdset);
        worker->max = worker->cmd_sck_id;

        timeout.tv_sec = SMTP_TMOUT_SEC;
        timeout.tv_usec = SMTP_TMOUT_USEC;
        ret = select(worker->max+1, &worker->rdset, NULL, NULL, &timeout);
        if (ret < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }

            LogError("errmsg:[%d] %s", errno, strerror(errno));
            return (void *)SMTP_ERR;
        }
        else if (0 == ret)
        {
            /* 超时: 模拟处理命令 */
            smtp_cmd_t cmd;
            smtp_cmd_work_t *work_cmd = (smtp_cmd_work_t *)&cmd.args;

            for (idx=0; idx<SMTP_WORKER_HDL_QNUM; ++idx)
            {
                memset(&cmd, 0, sizeof(cmd));

                cmd.type = SMTP_CMD_WORK;
                work_cmd->num = -1;
                work_cmd->rqidx = SMTP_WORKER_HDL_QNUM * worker->tidx + idx;

                smtp_worker_cmd_hdl(ctx, worker, &cmd);
            }
            continue;
        }

        /* 3. Call work handler */
        smtp_worker_core_handler(ctx, worker);
    }

    pthread_exit(NULL);
    return (void *)-1;
}

/******************************************************************************
 ** Name : smtp_worker_get_curr
 ** Desc : Get current worker context 
 ** Input: 
 **     ctx: Global context
 ** Output: NONE
 ** Return: Address of work context
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.03.28 #
 ******************************************************************************/
static smtp_worker_t *smtp_worker_get_curr(smtp_cntx_t *ctx)
{
    int tidx = 0;
    
    /* 1. Get thread idx */
    tidx = thread_pool_get_tidx(ctx->worktp);
    if (tidx < 0)
    {
        LogError("Get index of current thread failed!");
        return NULL;
    }

    return (smtp_worker_t *)(ctx->worktp->data + tidx * sizeof(smtp_worker_t));
}

/******************************************************************************
 ** Name : smtp_worker_init
 ** Desc : Init work context 
 ** Input: 
 **     ctx: Global context
 ** Output: NONE
 ** Return: Address of work context
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.03.28 #
 ******************************************************************************/
static int smtp_worker_init(smtp_cntx_t *ctx, smtp_worker_t *worker)
{
    char path[FILE_PATH_MAX_LEN];
    smtp_conf_t *conf = &ctx->conf; 

    /* 1. Create command fd */
    smtp_worker_usck_path(conf, path, worker->tidx);
    
    worker->cmd_sck_id = usck_udp_creat(path);
    if (worker->cmd_sck_id < 0)
    {
        LogError("Create unix-udp socket failed!");
        return SMTP_ERR;
    }

    return 0;
}

/******************************************************************************
 ** Name : smtp_worker_core_handler
 ** Desc : Core handler of work-svr
 ** Input: 
 **     ctx: Global context
 **     work: Work context
 ** Output: NONE
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.03.28 #
 ******************************************************************************/
static int smtp_worker_core_handler(smtp_cntx_t *ctx, smtp_worker_t *worker)
{
    int ret = 0;
    smtp_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));

    if (!FD_ISSET(worker->cmd_sck_id, &worker->rdset))
    {
        return SMTP_OK;
    }

    ret = usck_udp_recv(worker->cmd_sck_id, (void *)&cmd, sizeof(cmd));
    if (ret < 0)
    {
        LogError("Recv command failed! errmsg:[%d] %s", errno, strerror(errno));
        return SMTP_ERR_RECV_CMD;
    }

    switch (cmd.type)
    {
        case SMTP_CMD_WORK:
        {
            return smtp_worker_cmd_hdl(ctx, worker, &cmd);
        }
        default:
        {
            LogError("Received unknown command! type:[%d]", cmd.type);
            return SMTP_ERR_UNKNOWN_CMD;
        }
    }

    return SMTP_ERR_UNKNOWN_CMD;
}

/******************************************************************************
 ** Name : smtp_worker_cmd_hdl
 ** Desc : Handle WORK REQ
 ** Input: 
 **     ctx: Global context
 **     cmd: WORK_REQ command
 ** Output: NONE
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.04.11 #
 ******************************************************************************/
static int smtp_worker_cmd_hdl(smtp_cntx_t *ctx, smtp_worker_t *worker, const smtp_cmd_t *cmd)
{
    void *addr;
    queue_t *rq;
    smtp_header_t *head;
    smtp_reg_t *reg;
    smtp_cmd_work_t *work_cmd = (smtp_cmd_work_t *)&cmd->args;

    /* 1. 获取Recv队列地址 */
    rq = ctx->recvq[work_cmd->rqidx];
   
    while (1 || work_cmd->num-- > 0)
    {
        /* 2. 从Recv队列获取数据 */
        addr = queue_pop(rq, &addr);
        if (NULL == addr)
        {   
            /*log_debug(worker->log, "Didn't get data from queue!"); */
            return SMTP_OK;
        }
        
        /* 3. 执行回调函数 */
        head = (smtp_header_t *)addr;

        reg = &ctx->reg[head->type];

        reg->cb(head->type, addr+sizeof(smtp_header_t), head->body_len, reg->args);

        /* 4. 释放内存空间 */
        queue_dealloc(rq, addr);

        ++worker->work_data_num; /* 处理计数 */
    }

    return SMTP_OK;
}

/******************************************************************************
 ** Name : smtp_work_def_hdl
 ** Desc : Default work request handler
 ** Input: 
 **     type: Data type. Range:0~SMTP_TYPE_MAX
 **     buff: Data address
 **     len: Data length
 **     args: Additional parameter
 ** Output: NONE
 ** Return: 0:Succ !0:Fail
 ** Proc : 
 ** Note : 
 ** Author: # Qifeng.zou # 2014.03.31 #
 ******************************************************************************/
int smtp_work_def_hdl(unsigned int type, char *buff, size_t len, void *args)
{
    log_debug(worker->log, "Call %s() type:%d len:%d", __func__, type, len);

    return SMTP_OK;
}
