#include"ngx_mem_pool.h"
//#include"malloc.h"

//  创建指定size大小的内存池，但是小块内存池大小不超过一个页面大小
void* ngx_mem_pool::ngx_create_pool(size_t size) {
	pool = (ngx_pool_s*)malloc(size);//根据用户指定大小开辟内存池
	if (pool == nullptr) {
		return nullptr;
	}
	//  初始化ngx_pool_data_t的信息
	pool->d.last = (u_char*)pool + sizeof(ngx_pool_s); //  内存池起始
	pool->d.end = (u_char*)pool + size; //  内存池末尾
	pool->d.next = nullptr; //  下一个小块内存
	pool->d.failed = 0;
	//  初始化内存池管理成员信息
	size = size - sizeof(ngx_pool_s);
	pool->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL;//和页面大小进行比较

	pool->current = pool;
	pool->large = nullptr;
	pool->cleanup = nullptr;

	return pool;
}

//  考虑内存字节对齐，从内存池申请size大小的内存
void* ngx_mem_pool::ngx_palloc(size_t size) {
	if (size <= ngx_mem_pool::pool->max) {
		return ngx_palloc_small(size, 1);//小块内存分配
	}
	return ngx_palloc_large(size);//大块内存分配
}

//  不考虑内存字节对齐，从内存池申请size大小的内存
void* ngx_mem_pool::ngx_pnalloc(size_t size) {
	if (size <= pool->max) {
		return ngx_palloc_small(size, 0);
	}
	return ngx_palloc_large(size);
}

// 调用的是ngx_palloc实现内存分配，但是会初始化0
void* ngx_mem_pool::ngx_pcalloc(size_t size) {
	void* p;
	p = ngx_palloc(size);
	if (p) {
		ngx_memzero(p, size);   //buf缓冲区清零
	}

	return p;
}

//  小块内存分配，内存池不够则从操作系统开辟。align=1意味着需要内存对齐
void* ngx_mem_pool::ngx_palloc_small(size_t size, ngx_uint_t align) {
	u_char* m;
	ngx_pool_s* p;

	p = pool->current;

	do {
		m = p->d.last;

		if (align) { //  如果要求对齐
			m = ngx_align_ptr(m, NGX_ALIGNMENT); //  内存对齐：把指针调整到平台相关的4/8倍数。
		}
		//  内存池的空闲内存大于要申请的内存
		if ((size_t)(p->d.end - m) >= size) {
			//  m指针偏移size字节，即内存池给应用程序分配内存
			p->d.last = m + size;

			return m;
		}
		//  如果本block块剩余的不够size，那么顺着p->d.next向下走到第二个内存块block。
		p = p->d.next;
	} while (p);

	//  从pool->current开始 遍历完了 所有的block，也没找到够用的空闲内存
	//  那么就只能新开辟block
	return ngx_palloc_block(size);
}

//  从操作系统malloc开辟新的小块内存池。
//  ngx_palloc_small调用ngx_palloc_block。ngx_palloc_block底层调用ngx_memalign。
// 在unix平台下ngx_memalign就是ngx_alloc。（就是对malloc的浅封装）
void* ngx_mem_pool::ngx_palloc_block(size_t size) {
	u_char* m;
	size_t       psize;
	ngx_pool_s* p, * newpool;

	psize = (size_t)(pool->d.end - (u_char*)pool); //  块大小，和ngx_create_pool中的size一样，即跟第一块小块内存池一样大

	m = (u_char*)malloc(psize);
	if (m == nullptr) {
		return nullptr;
	}

	newpool = (ngx_pool_s*)m;

	newpool->d.end = m + psize;
	newpool->d.next = nullptr;
	newpool->d.failed = 0;

	// 加上内存头部数据信息的大小
	m += sizeof(ngx_pool_data_t);
	m = ngx_align_ptr(m, NGX_ALIGNMENT);
	//[m, m + size]是即将分配出去的内存
	newpool->d.last = m + size;

	//  由于进入到这个函数必然意味着之前的block都分配内存失败
	//  所以要把前几个block的fail次数都++;
	//  当一个block了=块分配内存失败次数
	//  当一个block块失败次数>4之后，就认为这块Block的剩余内存已经很少，之后请求小内存时就不从这块Block开始请求
	for (p = pool->current; p->d.next; p = p->d.next) {
		if (p->d.failed++ > 4) {
			pool->current = p->d.next;
		}
	}

	p->d.next = newpool;

	return m;
}

//  大块内存分配
void* ngx_mem_pool::ngx_palloc_large(size_t size) {
	void* p;
	ngx_uint_t         n;
	ngx_pool_large_s* large;

	p = malloc(size);
	if (p == nullptr) {
		return nullptr;
	}

	n = 0;

	for (large = pool->large; large; large = large->next) {
		//  如果遍历到的这块大内存池节点的头信息的alloc==nullptr
		//  意味着并这个ngx_pool_large_t没有管理一块大内存。
		//  所以直接由它管理
		if (large->alloc == nullptr) { //  先遍历有没有free过的大块内存头信息节点
			large->alloc = p;
			return p;
		}

		if (n++ > 3) {
			break;
		}
	}

	//  向小内存池申请一段内存用作大内存池头信息
	large = (ngx_pool_large_s*)ngx_palloc_small(sizeof(ngx_pool_large_s), 1);
	if (large == nullptr) {
		free(p);
		return nullptr;
	}

	//  将大内存池头插法插入large起始的链表中
	large->alloc = p;
	large->next = pool->large;
	pool->large = large;

	return p;
}

//  释放大块内存
void ngx_mem_pool::ngx_pfree(void* p) {
	ngx_pool_large_s* l;

	for (l = pool->large; l; l = l->next) {
		if (p == l->alloc) {
			free(l->alloc);
			l->alloc = nullptr;
			return;
		}
	}
}
//  小块内存 不释放。只是移动指针。
//  因为从实现方式上来看，就释放不了。我们通过移动last指针来分配内存。

//  内存重置函数
void ngx_mem_pool::ngx_reset_pool() {
	ngx_pool_s* p;
	ngx_pool_large_s* l;
	//先重置大内存
	for (l = pool->large; l; l = l->next) {
		if (l->alloc) {
			free(l->alloc);
		}
	}
	//再重置小内存
	/*重置内存时，第二块小内存只有头部信息，不需要sizeof(头部信息与管理信息)
	for (p = pool; p; p = p->d.next) {
		p->d.last = (u_char*)p + sizeof(ngx_pool_s);
		p->d.failed = 0;
	}*/

	//处理第一块内存池
	p = pool;
	p->d.last = (u_char*)p + sizeof(ngx_pool_s);
	p->d.failed = 0;

	//从第二块内存池开始循环到最后一个内存池
	for (p = p->d.next; p; p = p->d.next) {
		p->d.last = (u_char*)p + sizeof(ngx_pool_data_t);
		p->d.failed = 0;
	}

	pool->current = pool;
	pool->large = nullptr;
}

//  内存池销毁函数
void ngx_mem_pool::ngx_destroy_pool() {
	ngx_pool_s* p, * n;
	ngx_pool_large_s* l;
	ngx_pool_cleanup_s* c;

	//  遍历外部资源信息头链表， 调用预设置的回调函数handler，释放外部资源。
	for (c = pool->cleanup; c; c = c->next) {
		if (c->handler) {
			c->handler(c->data);
		}
	}

	//  遍历大块内存信息头，释放大块内存
	for (l = pool->large; l; l = l->next) {
		if (l->alloc) {
			free(l->alloc);
		}
	}

	//  遍历小块内存信息头，释放小块内村
	for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
		free(p);
		if (n == nullptr) {
			break;
		}
	}
}

//  添加回调清理操作函数
ngx_pool_cleanup_s* ngx_mem_pool::ngx_pool_cleanup_add(size_t size) {
	ngx_pool_cleanup_s* c;

	c = (ngx_pool_cleanup_s*)ngx_palloc(sizeof(ngx_pool_cleanup_s)); //创建清理操作的头信息的内存
	if (c == nullptr) {
		return nullptr;
	}

	if (size) {
		c->data = ngx_palloc(size);
		if (c->data == nullptr) {
			return nullptr;
		}
	}
	else {
		c->data = nullptr;
	}

	c->handler = nullptr;
	c->next = pool->cleanup;
	pool->cleanup = c;
	return c;
}