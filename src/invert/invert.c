/******************************************************************************
 ** Coypright(C) 2014-2024 Xundao technology Co., Ltd
 **
 ** 文件名: invert.c
 ** 版本号: 1.0
 ** 描  述: 倒排索引、倒排文件处理
 **         如: 创建、插入、查找、删除、归并等
 **     1. 单词词典: 使用平衡二叉树组织(原因: 查询会比修改删除更加频繁)
 **     2. 文档列表: 使用B树组织
 ** 作  者: # Qifeng.zou # 2015.04.29 #
 ******************************************************************************/
#include "hash.h"
#include "invert.h"
#include "syscall.h"

/******************************************************************************
 **函数名称: invert_dic_word_cmp
 **功    能: 比较单词的大小
 **输入参数: 
 **     word: 单词
 **     data: 与word进行比较的单词对应的数据
 **输出参数:
 **返    回: KEY值
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.28 #
 ******************************************************************************/
static int invert_dic_word_cmp(char *word, void *data)
{
    invt_word_item_t *item = (invt_word_item_t *)data;

    return strcmp(word, item->word.str);
}

/******************************************************************************
 **函数名称: invert_creat
 **功    能: 创建倒排对象
 **输入参数: 
 **     max: 数组长度
 **     log: 日志对象
 **输出参数: NONE
 **返    回: 倒排对象
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.28 #
 ******************************************************************************/
invt_cntx_t *invert_creat(int max, log_cycle_t *log)
{
    int idx;
    invt_cntx_t *ctx;
    avl_option_t option;

    /* > 创建对象 */
    ctx = (invt_cntx_t *)calloc(1, sizeof(invt_cntx_t));
    if (NULL == ctx)
    {
        log_error(log, "errmsg:[%d] %s!", errno, strerror(errno));
        return NULL;
    }

    ctx->mod = max;
    ctx->log = log;
    ctx->pool = (void *)NULL;
    ctx->alloc = mem_alloc;
    ctx->dealloc = mem_dealloc;

    /* > 创建单词词典 */
    ctx->dic = (avl_tree_t **)calloc(max, sizeof(avl_tree_t *));
    if (NULL == ctx->dic)
    {
        log_error(log, "errmsg:[%d] %s!", errno, strerror(errno));
        FREE(ctx);
        return NULL;
    }

    for (idx=0; idx<max; ++idx)
    {
        memset(&option, 0, sizeof(option));

        option.pool = (void *)NULL;
        option.alloc = mem_alloc;
        option.dealloc = mem_dealloc;

        ctx->dic[idx] = avl_creat(&option,
                            (key_cb_t)hash_time33_ex,
                            (avl_cmp_cb_t)invert_dic_word_cmp);
        if (NULL == ctx->dic[idx])
        {
            log_error(log, "Create btree failed! idx:%d", idx);
            return NULL;
        }
    }

    return ctx;
}

/******************************************************************************
 **函数名称: invt_word_add
 **功    能: 新建单词
 **输入参数: 
 **     ctx: 全局对象
 **     word: 单词
 **     len: 单词长度
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.29 #
 ******************************************************************************/
static invt_word_item_t *invt_word_add(invt_cntx_t *ctx, char *word, int len)
{
    int idx;
    btree_option_t option;
    invt_word_item_t *item;

    idx = hash_time33(word) % ctx->mod;

    /* > 创建数据对象 */
    item = ctx->alloc(ctx->pool, sizeof(invt_word_item_t));
    if (NULL == item)
    {
        log_error(ctx->log, "Alloc memory failed!");
        return NULL;
    }

    memset(item, 0, sizeof(invt_word_item_t));

    do
    {
        /* > 设置word标签 */
        item->word.str = ctx->alloc(ctx->pool, len + 1);
        if (NULL == item->word.str)
        {
            log_error(ctx->log, "Alloc memory failed!");
            break;
        }

        snprintf(item->word.str, len + 1, "%s", word);
        item->word.len = strlen(word);

        /* > 创建文档列表 */
        option.pool = (void *)NULL;
        option.alloc = mem_alloc;
        option.dealloc = mem_dealloc;

        item->doc_list = (btree_t *)btree_creat(3, &option);
        if (NULL == item->doc_list)
        {
            log_error(ctx->log, "Create btree failed! word:%s", word);
            break;
        }

        /* > 插入单词词典 */
        if (avl_insert(ctx->dic[idx], word, len, (void *)item))
        {
            log_error(ctx->log, "Insert avl failed! word:%s idx:%d", word, idx);
            break;
        }

        return item;
    } while(0);

    if (item->doc_list) { ctx->dealloc(ctx->pool, item->doc_list); }
    if (item->word.str) { ctx->dealloc(ctx->pool, item->word.str); }
    if (item) { ctx->dealloc(ctx->pool, item); }

    return NULL;
}

/******************************************************************************
 **函数名称: invt_word_add_doc
 **功    能: 添加文档列表
 **输入参数: 
 **     ctx: 全局对象
 **     word: 单词
 **     len: 单词长度
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.29 #
 ******************************************************************************/
static int invt_word_add_doc(invt_cntx_t *ctx, invt_word_item_t *item, const char *url, int freq)
{
    int len;
    invt_doc_t *doc;

    /* > 创建文档项 */
    doc = ctx->alloc(ctx->pool, sizeof(invt_doc_t));
    if (NULL == doc)
    {
        log_error(ctx->log, "Alloc memory failed!");
        return INVT_ERR;
    }

    len = strlen(url);
    doc->url.str = ctx->alloc(ctx->pool, len + 1);
    if (NULL == doc->url.str)
    {
        log_error(ctx->log, "Alloc memory failed!");
        ctx->dealloc(ctx->pool, doc);
        return INVT_ERR;
    }

    snprintf(doc->url.str, len+1, "%s", url);
    doc->url.len = len;
    doc->freq = freq;

    /* > 插入文档列表 */
    // btree_insert(item->doc_list, url, len,doc);

    return INVT_OK;
}

/******************************************************************************
 **函数名称: invert_insert
 **功    能: 插入倒排信息
 **输入参数: 
 **     ctx: 全局对象
 **     word: 关键字
 **     url: 包含关键字的文档
 **     freq: 词频
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.28 #
 ******************************************************************************/
int invert_insert(invt_cntx_t *ctx, char *word, const char *url, int freq)
{
    int idx;
    avl_node_t *node;
    invt_word_item_t *item;

    idx = hash_time33(word) % ctx->mod;

    /* > 查找单词项 */
    node = avl_query(ctx->dic[idx], word, strlen(word));
    if (NULL == node)
    {
        item = invt_word_add(ctx, word, strlen(word));
        if (NULL == item)
        {
            log_error(ctx->log, "Create word item failed!");
            return INVT_ERR;
        }
    }
    else
    {
        item = (invt_word_item_t *)node->data;
    }

    /* > 插入文档列表 */
    if (invt_word_add_doc(ctx, item, url, freq))
    {
        log_error(ctx->log, "Add document item failed!");
        return INVT_ERR;
    }
   
    return INVT_OK;
}

/******************************************************************************
 **函数名称: invert_query
 **功    能: 查询倒排信息
 **输入参数: 
 **     ctx: 全局对象
 **     word: 关键字
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.28 #
 ******************************************************************************/
invt_word_item_t *invert_query(invt_cntx_t *ctx, char *word)
{
    int idx;
    avl_node_t *node;

    idx = hash_time33(word) % ctx->mod;

    node = avl_query(ctx->dic[idx], word, strlen(word));
    if (NULL == node)
    {
        log_error(ctx->log, "Query word [%s] failed! idx:%d", word, idx);
        return NULL;
    }
    
    return (invt_word_item_t *)node->data;
}

/******************************************************************************
 **函数名称: invert_remove
 **功    能: 删除倒排信息
 **输入参数: 
 **     ctx: 全局对象
 **     word: 关键字
 **输出参数:
 **     item: 单词项数据
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.28 #
 ******************************************************************************/
int invert_remove(invt_cntx_t *ctx, char *word)
{
    int idx;
    invt_word_item_t *item;

    idx = hash_time33(word) % ctx->mod;

    if (avl_delete(ctx->dic[idx], word, strlen(word), (void **)&item))
    {
        log_error(ctx->log, "Query word [%s] failed! idx:%d", word, idx);
        return INVT_ERR;
    }

    btree_destroy(item->doc_list);

    ctx->dealloc(ctx->pool, item->word.str);
    ctx->dealloc(ctx->pool, item);
    
    return INVT_OK;
}

/******************************************************************************
 **函数名称: invert_destroy
 **功    能: 销毁倒排对象
 **输入参数: 
 **输出参数: NONE
 **返    回: 0:成功 !0:失败
 **实现描述: 
 **注意事项: 
 **作    者: # Qifeng.zou # 2015.04.28 #
 ******************************************************************************/
int invert_destroy(invt_cntx_t *ctx)
{
    int idx;

    for (idx=0; idx<ctx->mod; ++idx)
    {
        avl_destroy(ctx->dic[idx]);
    }

    FREE(ctx->dic);
    FREE(ctx);

    return 0;
}
