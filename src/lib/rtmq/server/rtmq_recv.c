/******************************************************************************
 ** Copyright(C) 2014-2024 Qiware technology Co., Ltd
 **
 ** 文件名: rtmq_recv.c
 ** 版本号: 1.0
 ** 描  述: 实时消息队列(Real-Time Message Queue)
 **         1. 主要用于异步系统之间数据消息的传输
 ** 作  者: # Qifeng.zou # 2014.12.29 #
 ******************************************************************************/

#include "log.h"
#include "lock.h"
#include "redo.h"
#include "shm_opt.h"
#include "rtmq_mesg.h"
#include "rtmq_comm.h"
#include "rtmq_recv.h"
#include "thread_pool.h"

static int rtmq_creat_recvq(rtmq_cntx_t *ctx);
static int rtmq_creat_sendq(rtmq_cntx_t *ctx);
static int rtmq_creat_distq(rtmq_cntx_t *ctx);

static int rtmq_cmd_send_dist_req(rtmq_cntx_t *ctx);

static int rtmq_creat_recvs(rtmq_cntx_t *ctx);
void rtmq_recvs_destroy(void *_ctx, void *param);

static int rtmq_creat_workers(rtmq_cntx_t *ctx);
void rtmq_workers_destroy(void *_ctx, void *param);

static int rtmq_lock_server(const rtmq_conf_t *conf);
static int rtmq_proc_def_hdl(int type, int orig, char *buff, size_t len, void *param);

/******************************************************************************
 **函数名称: rtmq_init
 **功    能: 初始化SDTP接收端
 **输入参数:
 **     conf: 配置信息
 **     log: 日志对象
 **输出参数: NONE
 **返    回: 全局对象
 **实现描述:
 **     1. 创建全局对象
 **     2. 备份配置信息
 **     3. 初始化接收端
 **注意事项:
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
rtmq_cntx_t *rtmq_init(const rtmq_conf_t *cf, log_cycle_t *log)
{
    rtmq_cntx_t *ctx;
    rtmq_conf_t *conf;
    char path[FILE_NAME_MAX_LEN];

    /* > 创建全局对象 */
    ctx = (rtmq_cntx_t *)calloc(1, sizeof(rtmq_cntx_t));
    if (NULL == ctx) {
        log_error(log, "errmsg:[%d] %s!", errno, strerror(errno));
        return NULL;
    }

    ctx->log = log;
    conf = &ctx->conf;
    memcpy(conf, cf, sizeof(rtmq_conf_t));  /* 配置信息 */
    conf->recvq_num = RTMQ_WORKER_HDL_QNUM * cf->work_thd_num;

    do {
        /* > 锁住指定文件(注: 防止路径和结点ID相同的配置) */
        if (rtmq_lock_server(conf)) {
            log_error(ctx->log, "Lock path failed!");
            break;
        }

        /* > 创建通信套接字 */
        rtmq_cli_unix_path(conf, path);

        ctx->cmd_sck_id = unix_udp_creat(path);
        if (ctx->cmd_sck_id < 0) {
            log_error(ctx->log, "Create command socket failed! path:%s", path);
            break;
        }

        spin_lock_init(&ctx->cmd_sck_lock);

        /* > 构建NODE->SVR映射表 */
        if (rtmq_node_to_svr_map_init(ctx)) {
            log_error(ctx->log, "Initialize sck-dev map table failed!");
            break;
        }

        /* > 初始化订阅列表 */
        if (rtmq_sub_mgr_init(&ctx->sub_mgr)) {
            log_error(ctx->log, "Initialize sub list failed!");
            break;
        }

        /* > 初始化注册信息 */
        ctx->reg = avl_creat(NULL, (key_cb_t)key_cb_int32, (cmp_cb_t)cmp_cb_int32);
        if (NULL == ctx->reg) {
            log_error(ctx->log, "Create register map failed!");
            break;
        }

        /* > 创建接收队列 */
        if (rtmq_creat_recvq(ctx)) {
            log_error(ctx->log, "Create recv queue failed!");
            break;
        }

        /* > 创建发送队列 */
        if (rtmq_creat_sendq(ctx)) {
            log_error(ctx->log, "Create send queue failed!");
            break;
        }

        /* > 创建分发队列 */
        if (rtmq_creat_distq(ctx)) {
            log_error(ctx->log, "Create distribute queue failed!");
            break;
        }

        /* > 创建接收线程池 */
        if (rtmq_creat_recvs(ctx)) {
            log_error(ctx->log, "Create recv thread pool failed!");
            break;
        }

        /* > 创建工作线程池 */
        if (rtmq_creat_workers(ctx)) {
            log_error(ctx->log, "Create worker thread pool failed!");
            break;
        }

        /* > 初始化侦听服务 */
        if (rtmq_lsn_init(ctx)) {
            log_error(ctx->log, "Create worker thread pool failed!");
            break;
        }

        return ctx;
    } while(0);

    close(ctx->cmd_sck_id);
    return NULL;
}

/******************************************************************************
 **函数名称: rtmq_launch
 **功    能: 启动SDTP接收端
 **输入参数:
 **     ctx: 全局对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
int rtmq_launch(rtmq_cntx_t *ctx)
{
    int idx;
    pthread_t tid;
    thread_pool_t *tp;
    rtmq_listen_t *lsn = &ctx->listen;

    /* > 设置接收线程回调 */
    tp = ctx->recvtp;
    for (idx=0; idx<tp->num; ++idx) {
        thread_pool_add_worker(tp, rtmq_rsvr_routine, ctx);
    }

    /* > 设置工作线程回调 */
    tp = ctx->worktp;
    for (idx=0; idx<tp->num; ++idx) {
        thread_pool_add_worker(tp, rtmq_worker_routine, ctx);
    }

    /* > 创建侦听线程 */
    if (thread_creat(&lsn->tid, rtmq_lsn_routine, ctx)) {
        log_error(ctx->log, "Start listen failed");
        return RTMQ_ERR;
    }

    /* > 创建分发线程 */
    if (thread_creat(&tid, rtmq_dsvr_routine, ctx)) {
        log_error(ctx->log, "Start distribute thread failed");
        return RTMQ_ERR;
    }

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_register
 **功    能: 消息处理的注册接口
 **输入参数:
 **     ctx: 全局对象
 **     type: 扩展消息类型 Range:(0 ~ RTMQ_TYPE_MAX)
 **     proc: 回调函数
 **     param: 附加参数
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项:
 **     1. 只能用于注册处理扩展数据类型的处理
 **     2. 不允许重复注册
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
int rtmq_register(rtmq_cntx_t *ctx, int type, rtmq_reg_cb_t proc, void *param)
{
    rtmq_reg_t *item;

    item = (rtmq_reg_t *)calloc(1, sizeof(rtmq_reg_t));
    if (NULL == item) {
        log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));
        return -1;
    }

    item->type = type;
    item->proc = proc;
    item->param = param;

    if (avl_insert(ctx->reg, &type, sizeof(type), item)) {
        log_error(ctx->log, "Register maybe repeat! type:%d!", type);
        free(item);
        return RTMQ_ERR_REPEAT_REG;
    }

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_async_send
 **功    能: 接收客户端发送数据
 **输入参数:
 **     ctx: 全局对象
 **     type: 消息类型
 **     dest: 目标结点ID
 **     data: 需要发送的数据
 **     len: 发送数据的长度
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 将数据放入应答队列
 **注意事项: 内存结构: 转发信息(frwd) + 实际数据
 **作    者: # Qifeng.zou # 2015.06.01 #
 ******************************************************************************/
int rtmq_async_send(rtmq_cntx_t *ctx, int type, int dest, void *data, size_t len)
{
    int idx;
    void *addr;
    rtmq_frwd_t *frwd;

    idx = rand() % ctx->conf.distq_num;

    /* > 申请队列空间 */
    addr = queue_malloc(ctx->distq[idx], sizeof(rtmq_frwd_t)+len);
    if (NULL == addr) {
        return RTMQ_ERR;
    }

    frwd = (rtmq_frwd_t *)addr;

    frwd->type = type; 
    frwd->dest = dest;
    frwd->length = len;

    memcpy(addr+sizeof(rtmq_frwd_t), data, len);

    /* > 压入队列空间 */
    if (queue_push(ctx->distq[idx], addr)) {
        queue_dealloc(ctx->distq[idx], addr);
        return RTMQ_ERR;
    }

    rtmq_cmd_send_dist_req(ctx);

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_creat_recvq
 **功    能: 创建接收队列
 **输入参数:
 **     ctx: 全局对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **     1. 创建队列数组
 **     2. 依次创建接收队列
 **注意事项:
 **作    者: # Qifeng.zou # 2014.12.30 #
 ******************************************************************************/
static int rtmq_creat_recvq(rtmq_cntx_t *ctx)
{
    int idx;
    rtmq_conf_t *conf = &ctx->conf;

    /* > 创建队列数组 */
    ctx->recvq = calloc(conf->recvq_num, sizeof(queue_t *));
    if (NULL == ctx->recvq) {
        log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));
        return RTMQ_ERR;
    }

    /* > 依次创建接收队列 */
    for(idx=0; idx<conf->recvq_num; ++idx) {
        ctx->recvq[idx] = queue_creat(conf->recvq.max, conf->recvq.size);
        if (NULL == ctx->recvq[idx]) {
            log_error(ctx->log, "Create queue failed! max:%d size:%d",
                    conf->recvq.max, conf->recvq.size);
            return RTMQ_ERR;
        }
    }

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_creat_sendq
 **功    能: 创建发送队列
 **输入参数:
 **     ctx: 全局对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2015.05.22 #
 ******************************************************************************/
static int rtmq_creat_sendq(rtmq_cntx_t *ctx)
{
    int idx;
    rtmq_conf_t *conf = &ctx->conf;

    /* > 创建队列数组 */
    ctx->sendq = calloc(1, conf->recv_thd_num*sizeof(queue_t *));
    if (NULL == ctx->sendq) {
        log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));
        return RTMQ_ERR;
    }

    /* > 依次创建发送队列 */
    for(idx=0; idx<conf->recv_thd_num; ++idx) {
        ctx->sendq[idx] = queue_creat(conf->sendq.max, conf->sendq.size);
        if (NULL == ctx->sendq[idx]) {
            log_error(ctx->log, "Create send-queue failed! max:%d size:%d",
                    conf->sendq.max, conf->sendq.size);
            return RTMQ_ERR;
        }
    }

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_creat_distq
 **功    能: 创建分发队列
 **输入参数:
 **     ctx: 全局对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2015-07-06 11:21:28 #
 ******************************************************************************/
static int rtmq_creat_distq(rtmq_cntx_t *ctx)
{
    int idx;
    rtmq_conf_t *conf = &ctx->conf;

    /* > 申请对象空间 */
    ctx->distq = (queue_t **)calloc(1, conf->distq_num*sizeof(queue_t *));
    if (NULL == ctx->distq) {
        log_error(ctx->log, "Alloc memory failed!");
        return RTMQ_ERR;
    }

    /* > 依次创建队列 */
    for (idx=0; idx<conf->distq_num; ++idx) {
        ctx->distq[idx] = queue_creat(conf->sendq.max, conf->sendq.size);
        if (NULL == ctx->distq[idx]) {
            log_error(ctx->log, "Create queue failed!");
            return RTMQ_ERR;
        }
    }

    return RTMQ_OK;
}



/******************************************************************************
 **函数名称: rtmq_creat_recvs
 **功    能: 创建接收线程池
 **输入参数:
 **     ctx: 全局对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **     1. 创建线程池
 **     2. 创建接收对象
 **     3. 初始化接收对象
 **注意事项:
 **作    者: # Qifeng.zou # 2015.01.01 #
 ******************************************************************************/
static int rtmq_creat_recvs(rtmq_cntx_t *ctx)
{
    int idx;
    rtmq_rsvr_t *rsvr;
    rtmq_conf_t *conf = &ctx->conf;

    /* > 创建接收对象 */
    rsvr = (rtmq_rsvr_t *)calloc(conf->recv_thd_num, sizeof(rtmq_rsvr_t));
    if (NULL == rsvr) {
        log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));
        return RTMQ_ERR;
    }

    /* > 创建线程池 */
    ctx->recvtp = thread_pool_init(conf->recv_thd_num, NULL, (void *)rsvr);
    if (NULL == ctx->recvtp) {
        log_error(ctx->log, "Initialize thread pool failed!");
        free(rsvr);
        return RTMQ_ERR;
    }

    /* > 初始化接收对象 */
    for (idx=0; idx<conf->recv_thd_num; ++idx) {
        if (rtmq_rsvr_init(ctx, rsvr+idx, idx)) {
            log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));

            free(rsvr);
            thread_pool_destroy(ctx->recvtp);
            ctx->recvtp = NULL;

            return RTMQ_ERR;
        }
    }

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_recvs_destroy
 **功    能: 销毁接收线程池
 **输入参数:
 **     ctx: 全局对象
 **     param: 附加参数
 **输出参数: NONE
 **返    回: VOID
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2015.01.01 #
 ******************************************************************************/
void rtmq_recvs_destroy(void *_ctx, void *param)
{
    int idx;
    rtmq_cntx_t *ctx = (rtmq_cntx_t *)_ctx;
    rtmq_rsvr_t *rsvr = (rtmq_rsvr_t *)ctx->recvtp->data;

    for (idx=0; idx<ctx->conf.recv_thd_num; ++idx, ++rsvr) {
        /* > 关闭命令套接字 */
        CLOSE(rsvr->cmd_sck_id);

        /* > 关闭通信套接字 */
        rtmq_rsvr_del_all_conn_hdl(ctx, rsvr);
    }

    FREE(ctx->recvtp->data);
    thread_pool_destroy(ctx->recvtp);

    return;
}

/******************************************************************************
 **函数名称: rtmq_creat_workers
 **功    能: 创建工作线程池
 **输入参数:
 **     ctx: 全局对象
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **     1. 创建线程池
 **     2. 创建工作对象
 **     3. 初始化工作对象
 **注意事项:
 **作    者: # Qifeng.zou # 2015.01.06 #
 ******************************************************************************/
static int rtmq_creat_workers(rtmq_cntx_t *ctx)
{
    int idx;
    rtmq_worker_t *wrk;
    rtmq_conf_t *conf = &ctx->conf;

    /* > 创建工作对象 */
    wrk = (void *)calloc(conf->work_thd_num, sizeof(rtmq_worker_t));
    if (NULL == wrk) {
        log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));
        return RTMQ_ERR;
    }

    /* > 创建线程池 */
    ctx->worktp = thread_pool_init(conf->work_thd_num, NULL, (void *)wrk);
    if (NULL == ctx->worktp) {
        log_error(ctx->log, "Initialize thread pool failed!");
        free(wrk);
        return RTMQ_ERR;
    }

    /* > 初始化工作对象 */
    for (idx=0; idx<conf->work_thd_num; ++idx) {
        if (rtmq_worker_init(ctx, wrk+idx, idx)) {
            log_error(ctx->log, "errmsg:[%d] %s!", errno, strerror(errno));
            free(wrk);
            thread_pool_destroy(ctx->recvtp);
            ctx->recvtp = NULL;
            return RTMQ_ERR;
        }
    }

    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_workers_destroy
 **功    能: 销毁工作线程池
 **输入参数:
 **     ctx: 全局对象
 **     param: 附加参数
 **输出参数: NONE
 **返    回: VOID
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2015.01.06 #
 ******************************************************************************/
void rtmq_workers_destroy(void *_ctx, void *param)
{
    int idx;
    rtmq_cntx_t *ctx = (rtmq_cntx_t *)_ctx;
    rtmq_conf_t *conf = &ctx->conf;
    rtmq_worker_t *wrk = (rtmq_worker_t *)ctx->worktp->data;

    for (idx=0; idx<conf->work_thd_num; ++idx, ++wrk) {
        CLOSE(wrk->cmd_sck_id);
    }

    FREE(ctx->worktp->data);
    thread_pool_destroy(ctx->recvtp);

    return;
}

/******************************************************************************
 **函数名称: rtmq_proc_def_hdl
 **功    能: 默认消息处理函数
 **输入参数:
 **     type: 消息类型
 **     orig: 源设备ID
 **     buff: 消息内容
 **     len: 内容长度
 **     param: 附加参数
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2015.01.06 #
 ******************************************************************************/
static int rtmq_proc_def_hdl(int type, int orig, char *buff, size_t len, void *param)
{
    return RTMQ_OK;
}

/******************************************************************************
 **函数名称: rtmq_cmd_send_dist_req
 **功    能: 通知分发服务
 **输入参数:
 **     cli: 上下文信息
 **     idx: 发送服务ID
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项:
 **作    者: # Qifeng.zou # 2015.03.20 #
 ******************************************************************************/
static int rtmq_cmd_send_dist_req(rtmq_cntx_t *ctx)
{
    int ret;
    rtmq_cmd_t cmd;
    char path[FILE_NAME_MAX_LEN];
    rtmq_conf_t *conf = &ctx->conf;

    if (spin_trylock(&ctx->cmd_sck_lock)) {
        return 0;
    }

    memset(&cmd, 0, sizeof(cmd));

    cmd.type = RTMQ_CMD_DIST_REQ;
    rtmq_dsvr_usck_path(conf, path);
    ret = unix_udp_send(ctx->cmd_sck_id, path, &cmd, sizeof(cmd));

    spin_unlock(&ctx->cmd_sck_lock);

    log_trace(ctx->log, "Send command %d! ret:%d", RTMQ_CMD_DIST_REQ, ret);

    return ret;
}

/******************************************************************************
 **函数名称: rtmq_lock_server
 **功    能: 锁定服务路径(注: 防止路径和结点ID相同的配置)
 **输入参数:
 **     conf: 配置信息
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述:
 **注意事项: 文件描述符可不关闭
 **作    者: # Qifeng.zou # 2016.05.02 21:22:21 #
 ******************************************************************************/
static int rtmq_lock_server(const rtmq_conf_t *conf)
{
    int fd;
    char path[FILE_NAME_MAX_LEN];

    rtmq_lock_path(conf, path);

    Mkdir2(path, DIR_MODE);

    fd = Open(path, O_CREAT|O_RDWR, OPEN_MODE);
    if (fd < 0) {
        return -1;
    }

    if (proc_try_wrlock(fd)) {
        CLOSE(fd);
        return -1;
    }

    return 0;
}
