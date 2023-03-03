#include <cstdlib>
#include <mutex>
#include <memory>
#include <iostream>
/*
* 移植SGI STL二级空间配置器源码
* 不同于nginx内存池，二级空间配置器中使用到的内存池需要注意多线程问题
* nginx内存池，可以一个线程创建一个内存池
* 而二级空间配置器，是用来给容器使用的，如vector对象。而一个vector对象很有可能在多个线程并发使用
* 移植成功！一个线程安全的二级空间配置器 my_allocator
*/

//  一级空间配置器处理>128B内存的分配 将对象构造和内存开辟分开
template<typename T>
class first_level_my_allocator
{
public:
	T* allocate(size_t size)
	{
		T* p = malloc(sizeof(T) * size);
		return p;
	}

	void deallocate(T* p)
	{
		free(p);
	}

	void construct(T* p, const T& _val)
	{
		new (p) T(_val);
	}

	void construct(T* p, T&& rval)
	{
		new (p) T(std::move(rval));
	}

	void destory(T* p)
	{
		p->~T();
	}
};

template <int __inst>
class __malloc_alloc_template {
private:
	static void* _S_oom_realloc(void*, size_t);
	//  预制的回调函数 _S_oom_malloc -> __malloc_alloc_oom_handler
	static void* _S_oom_malloc(size_t);
	static void (*__malloc_alloc_oom_handler)();
public:
	//  尝试分配内存
	static void* allocate(size_t __n)
	{
		void* __result = malloc(__n);
		//  尝试释放nbytes内存，返回给result
		if (0 == __result) __result = _S_oom_malloc(__n);
		return __result;
	}

	//  释放
	static void deallocate(void* __p, size_t /* __n */)
	{
		free(__p);
	}

	//  重新分配new_sz大小内存
	static void* reallocate(void* __p, size_t /* old_sz */, size_t __new_sz)
	{
		void* __result = realloc(__p, __new_sz);
		if (nullptr == __result) __result = _S_oom_realloc(__p, __new_sz);
		return __result;
	}

	//  用户通过这个接口来预制自己的回调函数。用以释放内存来解决内存不足的问题
	static void (*__set_malloc_handler(void (*__f)()))()
	{
		void (*__old)() = __malloc_alloc_oom_handler;
		__malloc_alloc_oom_handler = __f;
		return(__old);
	}
};

//  my_allocator中用到的__malloc_alloc_template中的两个函数和一个成员
template <int __inst>
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

template <int __inst>
void* __malloc_alloc_template<__inst>::_S_oom_realloc(void* __p, size_t __n)
{
	void (*__my_malloc_handler)();  //  用于接收回调函数
	void* __result;

	for (;;) {
		__my_malloc_handler = __malloc_alloc_oom_handler;  //  设置回调函数
		if (nullptr == __my_malloc_handler) { throw std::bad_alloc(); }  //  如果用户之前没设置回调函数，那么直接抛异常
		(*__my_malloc_handler)(); //  调用回调函数
		__result = realloc(__p, __n);  //  重新分配内存
		if (__result) return(__result);  //  如果成功，返回
	}
}

template <int __inst>
void (*__malloc_alloc_template<__inst>::__malloc_alloc_oom_handler)() = nullptr;
using malloc_alloc = __malloc_alloc_template<0>;

//  二级空间配置器 对外接口为allocate、deallocate、reallocate、construct、destroy
template<typename T>
class my_allocator
{
public:
	//不重要，配置相关信息使my_allocator不报错
	using value_type = T;
	using _Newfirst = T;
	using _From_primary = my_allocator;
	constexpr my_allocator() noexcept {}
	constexpr my_allocator(const my_allocator&) noexcept = default;
	template <class _Other>
	constexpr my_allocator(const my_allocator<_Other>&) noexcept {}

	//  开辟内存
	T* allocate(size_t __n);

	//  释放内存
	void deallocate(void* __p, size_t __n);

	//  重新分配内存。并将原先的内存归还给操作系统。并且不要再使用p的原指向地址
	void* reallocate(void* __p, size_t __old_sz, size_t __new_sz);

	//  构建对象
	void construct(T* __p, const T& val)
	{
		new (__p) T(val);
	}
	void construct(T* __p, T&& val)
	{
		new (__p) T(std::move(val));
	}

	//  释放内存
	void destroy(T* __p)
	{
		__p->~T();
	}

private:
	enum { _ALIGN = 8 };		//  8bytes为对齐方式，从8128Bytes一直扩充到128Bytes
	enum { _MAX_BYTES = 128 };	//  最大块大小   >128就不会放到内存池里了，也即不会用二级空间配置器。会用一级空间配置器
	enum { _NFREELISTS = 16 };  //  自由链表成员个数  _MAX_BYTES/_ALIGN

	//  将byte上调至8的倍数
	static size_t _S_round_up(size_t __bytes)
	{
		return (((__bytes)+(size_t)_ALIGN - 1) & ~((size_t)_ALIGN - 1));
	}

	//  计算出bytes大小的chunk应该挂载到自由链表free-list的哪个成员下
	static  size_t _S_freelist_index(size_t __bytes) {
		return (((__bytes)+(size_t)_ALIGN - 1) / (size_t)_ALIGN - 1);
	}

	//  开辟内存池，挂载到freelist成员下，返回请求的内存块。
	static void* _S_refill(size_t __n);

	//  从还没形成链表的原始内存中取内存分配给自由链表成员，去形成链表。如果原始空闲内存不够了，则再开辟
	static char* _S_chunk_alloc(size_t __size, int& __nobjs);

	//  每个chunk块的信息。
	union _Obj {
		union _Obj* _M_free_list_link;  //_M_free_list_link指向下一个chunk块
		char _M_client_data[1];
	};

	//  基于free-list的内存池，需要考虑线程安全
	static _Obj* volatile _S_free_list[_NFREELISTS];	//  16个元素的数组  防止被线程缓存
	static std::mutex mtx;

	//  static：类内声明、类外定义。
	static char* _S_start_free;         //   空闲free内存的起始start位置
	static char* _S_end_free;           //   空闲free内存的结束end位置 )
	static size_t _S_heap_size;         //   总共malloc过的内存大小（因为malloc是从堆heap上请求的，所以叫heapsize）
};

//  static 类内声明，类外定义/初始化
template<typename T>
std::mutex my_allocator<T>::mtx;

template <typename T>
char* my_allocator<T>::_S_start_free = nullptr;
template <typename T>
char* my_allocator<T>::_S_end_free = nullptr;
template <typename T>
size_t my_allocator<T>::_S_heap_size = 0;

template <typename T>
typename my_allocator<T>::_Obj* volatile
my_allocator<T> ::_S_free_list[my_allocator<T>::_NFREELISTS]
= { nullptr,nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, };

template<typename T>
T* my_allocator<T>::allocate(size_t __n)
{
	__n = __n * sizeof(T);		//  因为vector容器传来的是元素个数
	void* __ret = 0;
	//大于128B则用malloc不用内存池，调用一级空间配置器
	if (__n > (size_t)_MAX_BYTES) {
		__ret = malloc_alloc::allocate(__n); //分配内存，若失败用_S_oom_malloc尝试分配
	}
	//内存池分配内存
	else {
		_Obj* volatile* __my_free_list // 指向链表数组中的对应元素
			= _S_free_list + _S_freelist_index(__n);

		//  _S_free_list 是所有线程共享的。加锁实现线程安全
		std::lock_guard<std::mutex> guard(mtx);

		_Obj* __result = *__my_free_list;		// 获取指向数组中的值，即首个空闲chunk
		// 数组中空闲chunk为空
		if (__result == 0)
			__ret = _S_refill(_S_round_up(__n)); // 指向新分配内存池的首节点
		//进入内存池拿空闲chunk
		else {
			// 将数组中的值改为存放下一个空闲chunk的地址
			*__my_free_list = __result->_M_free_list_link;
			__ret = __result;
		}
	}
	return static_cast<T*>(__ret);
}

template<typename T>
void* my_allocator<T>::_S_refill(size_t __n)  //  n是一个chunk块的大小
{
	//  分配指定大小的内存池    __nobjs：chunk内存块数量 ；这里的 __n：每个chunk内存块大小。
	int __nobjs = 20;
	char* __chunk = _S_chunk_alloc(__n, __nobjs); // 指向分配好的__nobjs个chunk块的起始地址
	_Obj* volatile* __my_free_list; //遍历指针数组S_free_list的指针
	_Obj* __result;
	_Obj* __current_obj;	//chunk块连接用指针
	_Obj* __next_obj;		//chunk块连接用指针
	int __i;
	//  __nobjs：申请到的chunk块数量。当只申请到一个时，直接返回该内存块给上一级使用。无需建立各个chunk的连接关系，无需挂载到相应的freelist成员下。（因为只有一个）
	if (1 == __nobjs) return(__chunk);

	__my_free_list = _S_free_list + _S_freelist_index(__n);		//  根据内存块大小求出内存池应该在由freelist第几个成员管理（指向）

	//  静态链表：把每个chunk块通过Obj*里的指针连接起来
	//  每个内存块，有一部分的内存时union联合体Obj，里面有一个Obj*指针，负责连接每个空闲内存块。
	__result = (_Obj*)__chunk; //  __result：第一个内存块
	*__my_free_list = __next_obj = (_Obj*)(__chunk + __n);    //  改变指向为下一个空闲chunk
	//  仅初始化__nobjs个块成静态链表，使前一个chunk指向后一个chunk备用块不处理
	//   维护内存块间的连接  char* 因此+__n是偏移n个bytes +n是为了一次跑一个chunk块
	for (__i = 1; ; __i++) {
		__current_obj = __next_obj;
		__next_obj = (_Obj*)((char*)__next_obj + __n);
		//   最后一个chunk节点
		if (__nobjs - 1 == __i) {
			//   最后chunk块的next置空
			__current_obj->_M_free_list_link = nullptr;
			break;
		}
		// 非最后一个节点，连接chunk块
		else {
			__current_obj->_M_free_list_link = __next_obj;
		}
	}
	return(__result);   //  返回第一个空闲chunk块
}

template <typename T>
char* my_allocator<T>::_S_chunk_alloc(size_t __size, int& __nobjs)  //  __size：一个chunk块的大小 ；__nobjs：chunk内存块数量
{
	char* __result;
	size_t __total_bytes = __size * __nobjs;            //  本次总共需要请求的内存大小
	size_t __bytes_left = _S_end_free - _S_start_free;  //  __default_alloc_template<__threads, __inst> 从开始到现在，请求的剩余空闲的的内存大小。不包括回收的。只是开辟的。
	//  剩余的备用内存够支付本次请求的内存大小。
	if (__bytes_left >= __total_bytes) {
		__result = _S_start_free;                       //  __result 作为返回内存首地址
		_S_start_free += __total_bytes;                 //  移动_S_start_free
		return(__result);                               //  [result , _S_start_free)  作为请求结果返回
	}
	//  剩余的备用内存不够支付total，但起码能支付一个chunk。（因为要返回的至少是一个内存块大小）
	else if (__bytes_left >= __size) {
		__nobjs = (int)(__bytes_left / __size);  // 计算能够几个chunk
		__total_bytes = __size * __nobjs;
		__result = _S_start_free;
		_S_start_free += __total_bytes;  //重新标记空闲块起始位置
		return(__result);  //  返回已分配的__nobjs个chunk的起始位置
	}
	//  剩余的free内存连一个内存块也不够支付
	else {
		size_t __bytes_to_get =                         //  当剩余的空闲内存不够时，需要向操统malloc内存。这是计算出需要malloc内存的大小（至少malloc出来要求内存的(__total_bytes)两倍）
			2 * __total_bytes + _S_round_up(_S_heap_size >> 4);

		// 剩余的备用内存bytes_left,又不够本次请求的一个chunk块大小，就把这块内存挂载到他能所属的freelist成员下。（头插法）
		if (__bytes_left > 0) {                         //
			_Obj* volatile* __my_free_list =
				_S_free_list + _S_freelist_index(__bytes_left);

			((_Obj*)_S_start_free)->_M_free_list_link = *__my_free_list;
			*__my_free_list = (_Obj*)_S_start_free;
		}
		_S_start_free = (char*)malloc(__bytes_to_get);  //  向操统malloc内存
		if (nullptr == _S_start_free) {                       //  malloc失败
			size_t __i;
			_Obj* volatile* __my_free_list;
			_Obj* __p;
			//  从别的freelist成员管理的原始备用内存池中借用至少size大小的chunk块
			for (__i = __size; __i <= (size_t)_MAX_BYTES; __i += (size_t)_ALIGN)
			{
				__my_free_list = _S_free_list + _S_freelist_index(__i);
				__p = *__my_free_list;
				if (nullptr != __p) {  // 借用_S_free_list数组中指向的第一个块
					*__my_free_list = __p->_M_free_list_link;
					_S_start_free = (char*)__p;
					_S_end_free = _S_start_free + __i;
					return(_S_chunk_alloc(__size, __nobjs)); //  借用更大的块，在递归中能支付一个块，将大块继续分割
				}
			}
			//  都没有时，只能allcoate
			_S_end_free = nullptr;
			_S_start_free = (char*)malloc_alloc::allocate(__bytes_to_get);
		}// end if (nullptr == _S_start_free)
		// 若malloc成功
		_S_heap_size += __bytes_to_get;               //  _S_heap_size：迄今为止总共malloc了多少内存
		_S_end_free = _S_start_free + __bytes_to_get; //  移动_S_end_free指针。指向空闲内存块末尾
		return(_S_chunk_alloc(__size, __nobjs));      //  递归调用
	}
}

template<typename T>
void my_allocator<T>::deallocate(void* __p, size_t __n)		//  因为vector容器传来的是字节大小
{
	if (__n > (size_t)_MAX_BYTES)	//  n>128 同一级空间配置器
	{
		malloc_alloc::deallocate(__p, __n);
	}
	else
	{
		_Obj* volatile* __my_free_list
			= _S_free_list + _S_freelist_index(__n);  //  找到相应freelist成员
		_Obj* __q = (_Obj*)__p;       //  q指向要释放的内存块

		// acquire lock
		std::lock_guard<std::mutex> guard(mtx);

		__q->_M_free_list_link = *__my_free_list; // 头插法，将q插入到空闲静态链表中
		*__my_free_list = __q;
		// lock is released here
	}
}

template<typename T>
void* my_allocator<T>::reallocate(void* __p, size_t __old_sz, size_t __new_sz)
{
	void* __result;
	size_t __copy_sz;

	//  old new都>128byets，那么用的就应当是和一级空间配置器一样的方法malloc
	if (__old_sz > (size_t)_MAX_BYTES && __new_sz > (size_t)_MAX_BYTES) {
		return(realloc(__p, __new_sz));
	}

	//  如果即将分配的chunk块大小一致，那么不必，直接返回
	if (_S_round_up(__old_sz) == _S_round_up(__new_sz)) return(__p);

	//  重新开辟new_size大小内存，并拷贝数据到新开辟的内存里
	__result = allocate(__new_sz);
	__copy_sz = __new_sz > __old_sz ? __old_sz : __new_sz;
	memcpy(__result, __p, __copy_sz); //  拷贝数据
	deallocate(__p, __old_sz);  //  释放
	return(__result);
}