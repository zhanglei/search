/******************************************************************************
 ** Coypright(C) 2014-2024 Xundao technology Co., Ltd
 **
 ** 文件名: flt_push.c
 ** 版本号: 1.0
 ** 描  述: 任务推送
 **         负责任务推送到REDIS队列
 ** 作  者: # Qifeng.zou # 2015.03.14 #
 ******************************************************************************/

#include "log.h"
#include "comm.h"
#include "filter.h"
#include "flt_priv.h"
#include "syscall.h"
#include "flt_conf.h"

/******************************************************************************
 **函数名称: flwtr_push_routine
 **功    能: 将任务推送至REDIS队列
 **输入参数: 
 **     _ctx: 全局对象
 **输出参数:
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.03.14 #
 ******************************************************************************/
void *flt_push_routine(void *_ctx)
{
    redisReply *r; 
    flt_crwl_t *crwl;
    flt_cntx_t *ctx = (flt_cntx_t *)_ctx;
    flt_conf_t *conf = ctx->conf;

    while (1)
    {
        /* > 是否超过阈值 */
        if (redis_llen(ctx->redis->redis[REDIS_MASTER_IDX], conf->redis.taskq) > FLT_REDIS_UNDO_LIMIT_NUM)
        {
            Sleep(1);
            continue;
        }

        /* > 获取任务数据 */
        crwl = queue_pop(ctx->crwlq);
        if (NULL == crwl)
        {
            Sleep(1);
            continue;
        }

        /* > 进行网页处理 */
        r = redis_rpush(ctx->redis->redis[REDIS_MASTER_IDX],
                ctx->conf->redis.taskq, crwl->task_str);
        if (NULL == r
            || REDIS_REPLY_NIL == r->type)
        {
            if (r) { freeReplyObject(r); }
            log_error(ctx->log, "Push into undo task queue failed!");
            return (void *)-1;
        }

        /* > 释放空间 */
        queue_dealloc(ctx->crwlq, crwl);
        freeReplyObject(r);
    }

    return (void *)-1;
}
