		这是一个学习nginx内存池和SGI STL内存池的项目，对代码进行了详细的注释，发现了一些可能存在的小问题并给出了解决思路。欢迎大家阅读交流。

# ngx_mem_pool改进

## ngx_mem_pool头部信息

```c++
/*ngx内存池的头部信息和管理成员信息*/
struct ngx_pool_s {
	ngx_pool_data_t       d;			//存储当前小块内存的存储情况
	size_t                max;			//存储小块内存和大块内存的分界线
	ngx_pool_s*			  current;      //指向第一个可以提供小块内存分配的小块内存池
	ngx_pool_large_s*	  large;		//指向大块内存（链表）的入口地址
	ngx_pool_cleanup_s*	  cleanup;		//指向所有预置的清理操作回调函数（链表）的入口
};

struct ngx_pool_data_t {
	u_char*			last;	//小块内存池可用内存起始地址
	u_char*			end;	//小块内存池可用内存的末尾地址
	ngx_pool_s*		next;	//所有小块内存池都被串在一条链表上
	ngx_uint_t      failed; //记录了当前小块内存池分配失败的次数
};

struct ngx_pool_cleanup_s {
	ngx_pool_cleanup_pt		handler; 	  //定义一个函数指针，保存清理操作的回调函数
	void*					data;   	 //传给回调函数的参数的地址
	ngx_pool_cleanup_s*		next;   	  //所有的cleanup清理操作都被串在一条链表上
};

/*大块内存的头部信息*/
struct ngx_pool_large_s {
	ngx_pool_large_s*	next;		//所有的大块内存分配也是被串在一条链表上
	void*				alloc;		//保存分配出去的大块内存的起始地址
};
```

![image](https://github.com/SuNaKi/mem_pool/blob/master/ngx.png)

nginx大块内存分配 -> 内存释放ngx_free函数
nginx小块内存分配 -> 没有提供任何的内存释放函数，实际上，从小块内存的分配方式来看（直接通过last指针偏移来分配内存)，它也没法进行小块内存的回收。

## nginx本质: http服务器

​		是一个短链接的服务器，客户端（浏览器）发起一个request请求，到达nginx服务器以后，处理完成，nginx给客户端返回一个response响应，http服务器就主动断开tcp连接（http 1.1 keep-avlie: 60s)
​		http服务器(nginx）返回响应以后，需要等待60s，60s之内客户端又发来请求，重置这个时间，否则60s之内没有客户端发来的响应，nginx就主动断开连接，此时nginx可以调用ngx_reset_pool重置内存池了，等待下一次该客户端的请求

## 存在问题

​	在ngx_reset_pool() 函数中重置内存池存在一定空间的浪费，浪费了从第二块内存池开始至最后一块内存池中N*（sizeof（ngx_pool_s） -  sizeof（ngx_pool_data_t））大小的空间

```c++

	/*重置内存时，第二块小内存只有头部信息，不需要sizeof(头部信息与管理信息)*/
	for (p = pool; p; p = p->d.next) {
		p->d.last = (u_char*)p + sizeof(ngx_pool_s);
		p->d.failed = 0;
	}
```

将上面的代码改为如下代码即可在重置内存池时避免浪费

```c++
//重置第一块内存池
p = pool;
p->d.last = (u_char*)p + sizeof(ngx_pool_s);
p->d.failed = 0;

//从第二块内存池开始循环重置到最后一个内存池
for (p = p->d.next; p; p = p->d.next) {
	p->d.last = (u_char*)p + sizeof(ngx_pool_data_t);
	p->d.failed = 0;
}
```
# SGT_STL_pool改进

## SGT_STL_pool基本结构

```c++
static char* _S_start_free;    //   空闲free内存的起始start位置 
static char* _S_end_free;      //   空闲free内存的结束end位置 
static size_t _S_heap_size;    //   总共malloc过的内存大小（因为malloc是从堆heap上请求的，所以叫heapsize）

//  每个chunk块的信息
union _Obj { 
	union _Obj* _M_free_list_link;  //_M_free_list_link指向下一个chunk块
	char _M_client_data[1];			
};

enum { _ALIGN = 8 };		//  8bytes为对齐方式，从8128Bytes一直扩充到128Bytes
enum { _MAX_BYTES = 128 };	//  最大块大小>128就不会放到内存池里了，即不会用二级空间配置器，用一级空间配置器
enum { _NFREELISTS = 16 };  //  自由链表成员个数  _MAX_BYTES/_ALIGN

static _Obj* volatile _S_free_list[_NFREELISTS];	//  16个元素的数组  
_Obj* volatile* __my_free_list 					   // 指向数组中的对应元素
```

![image](https://github.com/SuNaKi/mem_pool/blob/master/SGI_STL.png)

## SGI STL二级空间配置器内存池的实现优点:

​	1.对于每一个字节数的chunk块分配，都是给出一部分进行使用，另一部分作为备用，这个备用可以给当前字节数使用，也可以给其它字节数使用

​	2.对于备用内存池划分完chunk块以后，如果还有剩余的很小的内存块，再次分配的时候，会把这些小的内
存块再次分配出去，备用内存池使用的干干净净!

​	3.当指定字节数内存分配失败以后，有一个异常处理的过程，bytes - 128字节所有的chunk块进行查看，如果哪个字节数有空闲的chunk块，直接借一个出去
​	如果上面操作失败，还会调用oom_malloc这么一个预先设置好的malloc内存分配失败以后的回调函数

​	没设置则会抛出 bad_alloc 异常
设置了则会循环调用回调函数直到分配出空间为止for(;;)(*oom_malloc_handler)() ;

## 存在问题

​	若回调函数无法有效清理内存空间释放出可分配的空间则会陷入死循环，浪费资源，可以设置回调函数最大重试次数，超过该次数则不再调用回调函数并抛出 bad_alloc 异常

```c++
void* __malloc_alloc_template<__inst>::_S_oom_malloc(size_t __n)
{
    void (*__my_malloc_handler)();    //回调函数
    void* __result;                   //返回分配的内存地址
    constexpr size_t max = 5;         //设置重复尝试次数
    for (size_t count = 0; count < max; count++) {
        __my_malloc_handler = __malloc_alloc_oom_handler;
        if (nullptr == __my_malloc_handler) { throw std::bad_alloc(); }
        (*__my_malloc_handler)();    //调用回调函数
        __result = malloc(__n);      //尝试分配内存
        if (__result) return(__result);
    }
    throw std::bad_alloc();
}

```

