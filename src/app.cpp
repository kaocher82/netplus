#include <fstream>

#include "../3rd/nlohmann/include/nlohmann/json.hpp"

#include <signal.h>

#include <netp/CPUID.hpp>
#include <netp/logger_broker.hpp>
#include <netp/io_event_loop.hpp>
#include <netp/dns_resolver.hpp>
#include <netp/socket.hpp>
#include <netp/logger/console_logger.hpp>
#include <netp/logger/file_logger.hpp>
#include <netp/os/api_wrapper.hpp>
#include <netp/signal_broker.hpp>

#include <netp/security/crc.hpp>
#include <netp/security/fletcher.hpp>

#include <netp/benchmark.hpp>

#ifdef _NETP_WIN
	#include <netp/os/winsock_helper.hpp>
#endif

#include <netp/app.hpp>

namespace netp {
	NRP<netp::signal_broker> g_sigbroker;

	static void signal_broker_init() {
		NETP_ASSERT(g_sigbroker == nullptr);
		g_sigbroker = netp::make_ref<netp::signal_broker>();
	}

	static void signal_broker_deinit() {
		NETP_ASSERT(g_sigbroker != nullptr);
		g_sigbroker = nullptr;
	}

	static void signal_void(int) {}

	static void signal_from_system(int signo) {
		NETP_ASSERT(g_sigbroker != nullptr);
		g_sigbroker->signal_raise(signo);
	}

	i64_t signal_register(int signo, netp::fn_signal_handler_t&& H) {
		if (H == nullptr) {
			NETP_WARN("[signal_broker]register_signal, ignore: %d", signo);
			::signal(signo, SIG_IGN);
			return -1;
		}

		::signal(signo, &signal_from_system);
		return g_sigbroker->bind(signo, std::forward<netp::fn_signal_handler_t>(H));
	}

	void signal_unregister(int signo, i64_t handle_id) {
		::signal(signo, &signal_void);
		g_sigbroker->unbind_by_handle_id(handle_id);
	}

	app_thread_init::app_thread_init() 
	{
		static_assert(sizeof(netp::i8_t) == 1, "assert sizeof(i8_t) failed");
		static_assert(sizeof(netp::u8_t) == 1, "assert sizeof(u8_t) failed");
		static_assert(sizeof(netp::i16_t) == 2, "assert sizeof(i16_t) failed");
		static_assert(sizeof(netp::u16_t) == 2, "assert sizeof(u16_t) failed");
		static_assert(sizeof(netp::i32_t) == 4, "assert sizeof(i32_t) failed");
		static_assert(sizeof(netp::u32_t) == 4, "assert sizeof(u32_t) failed");
		static_assert(sizeof(netp::i64_t) == 8, "assert sizeof(i64_t) failed");
		static_assert(sizeof(netp::u64_t) == 8, "assert sizeof(u64_t) failed");

#if __NETP_IS_BIG_ENDIAN
		NETP_ASSERT(netp::is_big_endian());
#else
		NETP_ASSERT(netp::is_little_endian());
#endif

#ifdef _NETP_WIN
		//CPU ia-64 is big endian, but windows run on a little endian mode on ia-64
		static_assert(!(__NETP_IS_BIG_ENDIAN), "windows always run on little endian");
#endif

		netp::random_init_seed();
		netp::tls_create<netp::impl::thread_data>();

#ifdef NETP_MEMORY_USE_TLS_POOL
		netp::global_pool_aligned_allocator::instance();
		netp::tls_create<netp::pool_aligned_allocator_t>();
#endif

#if defined(_DEBUG_MUTEX) || defined(_DEBUG_SHARED_MUTEX)
		//for mutex/lock deubg
		netp::tls_create<netp::mutex_set_t>();
#endif
	}

	app_thread_init::~app_thread_init()
	{
#if defined(_DEBUG_MUTEX) || defined(_DEBUG_SHARED_MUTEX)
		tls_destroy<mutex_set_t>();
#endif
		netp::tls_destroy<netp::impl::thread_data>();

#ifdef NETP_MEMORY_USE_TLS_POOL
		netp::tls_destroy<netp::pool_aligned_allocator_t>();
		netp::global_pool_aligned_allocator::instance()->destroy_instance();
#endif
	}

	void app_cfg::__init_from_cfg_json(const char* jsonfile) {
		std::ifstream ifs(jsonfile, std::ifstream::in);
		if (!ifs.good()) {
			return;
		}
		nlohmann::json cfg_json = nlohmann::json::parse(ifs, nullptr, false);
		ifs.close();

		if (cfg_json.is_discarded()) {
			return;
		}

		if (cfg_json.find("log") != cfg_json.end() && cfg_json["log"].is_string()) {
			cfg_log_filepathname(cfg_json["log"].get<std::string>());
		}

		if (cfg_json.find("def_loop_max_count") != cfg_json.end()) {
			cfg_poller_max(NETP_DEFAULT_POLLER_TYPE, cfg_json["def_loop_max_count"].get<int>());
		}

		if (cfg_json.find("def_loop_channel_buf") != cfg_json.end()) {
			cfg_channel_buf(NETP_DEFAULT_POLLER_TYPE, cfg_json["def_loop_channel_buf"].get<int>());
		}
	}

	void app_cfg::__parse_cfg(int argc, char** argv) {
	
		struct ::option long_options[] = {
			{"netp-cfg", optional_argument, 0, 1},
			{0,0,0,0}
		};

		::optind = 1;
		const char* optstring = "H:h::";
		int opt;
		int opt_idx;
		while ((opt = getopt_long(argc, argv, optstring, long_options, &opt_idx)) != -1) {
			switch (opt) {
			case 1:
			{
				__init_from_cfg_json(optarg);
			}
			break;
			}
		}
	}

	app::app(app_cfg const& cfg = app_cfg() ) :
		m_should_exit(false),
		m_cfg(cfg),
		m_app_startup_prev(cfg.app_startup_prev),
		m_app_startup_post(cfg.app_startup_post),
		m_app_exit_prev(cfg.app_exit_prev),
		m_app_exit_post(cfg.app_exit_post),
		m_app_event_loop_init_prev(cfg.app_event_loop_init_prev),
		m_app_event_loop_init_post(cfg.app_event_loop_init_post),
		m_app_event_loop_deinit_prev(cfg.app_event_loop_deinit_prev),
		m_app_event_loop_deinit_post(cfg.app_event_loop_deinit_post)
	{
		app_thread_init::instance();
		if (m_app_startup_prev) {
			m_app_startup_prev();
		}
		_startup();
		if (m_app_startup_post ) {
			m_app_startup_post();
		}
	}

	app::~app()
	{
		if (m_app_exit_prev ) {
			m_app_exit_prev();
		}
		_exit();
		if (m_app_exit_post ) {
			 m_app_exit_post();
		}
		//quick destroy
		//app_thread_init::instance()->destroy_instance();
	}

	void app::_startup() {
		__log_init();

#ifdef _NETP_DEBUG
		dump_arch_info();
#endif
		
#ifdef NETP_DEBUG_OBJECT_SIZE
		__dump_sizeof();
#endif

		NRP<app_test_unit> apptest = netp::make_ref<app_test_unit>();
		if (!apptest->run()) {
			NETP_INFO("apptest failed");
			exit(-2);
		}

		__signal_init();
		__net_init();
	}

	void app::_exit() {
		__net_deinit();
		__signal_deinit();
		__log_deinit();
	}

	void app::__log_init() {
		netp::logger_broker::instance()->init();
#ifdef NETP_ENABLE_CONSOLE_LOGGER
		NRP<logger::console_logger> clg = netp::make_ref <logger::console_logger>();
		clg->set_mask_by_level(NETP_CONSOLE_LOGGER_LEVEL);
		netp::logger_broker::instance()->add(clg);
#endif
		string_t logfilepath = "./netp.log";
		if (m_cfg.logfilepathname.length()) {
			logfilepath = netp::string_t(m_cfg.logfilepathname.c_str());
		}
		NRP<logger::file_logger> filelogger = netp::make_ref<netp::logger::file_logger>(logfilepath);
		filelogger->set_mask_by_level(NETP_FILE_LOGGER_LEVEL);
		netp::logger_broker::instance()->add(filelogger);

#ifdef NETP_ENABLE_ANDROID_LOG
		NRP<android_log_print> alp = netp::make_ref<android_log_print>();
		alp->set_mask_by_level(NETP_CONSOLE_LOGGER_LEVEL);
		__android_log_print(ANDROID_LOG_INFO, "[NETP]", "add android_log_print done");
		netp::logger_broker::instance()->add(alp);
#endif
	}
	void app::__log_deinit() {
		netp::logger_broker::instance()->deinit();
	}

	void app::dump_arch_info() {
		const char bob[2] = { 0x01, 0x02 };
		const u16_t bob_u16 = *((u16_t*)(&bob[0]));
#if __NETP_IS_BIG_ENDIAN
		std::string arch_info = "endian: big_endian\n" ;
		NETP_ASSERT(bob_u16 == ((0x01<<8)|0x02));
#else
		std::string arch_info = "endian: little_endian\n";
		NETP_ASSERT(bob_u16 == ((0x02<<8)|0x01));
#endif

		arch_info += "vender: " + netp::CPUID::Vendor() + "\n";
		arch_info += "brand: " + netp::CPUID::Brand() + "\n";
		arch_info += "instructions:";
		auto support_message = [](std::string& archinfo, const char* isa_feature, bool is_supported) {
			if (is_supported) {
				archinfo += std::string(" ") + isa_feature ;
			}
		};

		support_message(arch_info, "3DNOW", netp::CPUID::_3DNOW());
		support_message(arch_info, "3DNOWEXT", netp::CPUID::_3DNOWEXT());
		support_message(arch_info, "ABM", netp::CPUID::ABM());
		support_message(arch_info, "ADX", netp::CPUID::ADX());
		support_message(arch_info, "AES", netp::CPUID::AES());
		support_message(arch_info, "AVX", netp::CPUID::AVX());
		support_message(arch_info, "AVX2", netp::CPUID::AVX2());
		support_message(arch_info, "AVX512CD", netp::CPUID::AVX512CD());
		support_message(arch_info, "AVX512ER", netp::CPUID::AVX512ER());
		support_message(arch_info, "AVX512F", netp::CPUID::AVX512F());
		support_message(arch_info, "AVX512PF", netp::CPUID::AVX512PF());
		support_message(arch_info, "BMI1", netp::CPUID::BMI1());
		support_message(arch_info, "BMI2", netp::CPUID::BMI2());
		support_message(arch_info, "CLFSH", netp::CPUID::CLFSH());
		support_message(arch_info, "CMPXCHG16B", netp::CPUID::CMPXCHG16B());
		support_message(arch_info, "CX8", netp::CPUID::CX8());
		support_message(arch_info, "ERMS", netp::CPUID::ERMS());
		support_message(arch_info, "F16C", netp::CPUID::F16C());
		support_message(arch_info, "FMA", netp::CPUID::FMA());
		support_message(arch_info, "FSGSBASE", netp::CPUID::FSGSBASE());
		support_message(arch_info, "FXSR", netp::CPUID::FXSR());
		support_message(arch_info, "HLE", netp::CPUID::HLE());
		support_message(arch_info, "INVPCID", netp::CPUID::INVPCID());
		support_message(arch_info, "LAHF", netp::CPUID::LAHF());
		support_message(arch_info, "LZCNT", netp::CPUID::LZCNT());
		support_message(arch_info, "MMX", netp::CPUID::MMX());
		support_message(arch_info, "MMXEXT", netp::CPUID::MMXEXT());
		support_message(arch_info, "MONITOR", netp::CPUID::MONITOR());
		support_message(arch_info, "MOVBE", netp::CPUID::MOVBE());
		support_message(arch_info, "MSR", netp::CPUID::MSR());
		support_message(arch_info, "OSXSAVE", netp::CPUID::OSXSAVE());
		support_message(arch_info, "PCLMULQDQ", netp::CPUID::PCLMULQDQ());
		support_message(arch_info, "POPCNT", netp::CPUID::POPCNT());
		support_message(arch_info, "PREFETCHWT1", netp::CPUID::PREFETCHWT1());
		support_message(arch_info, "RDRAND", netp::CPUID::RDRAND());
		support_message(arch_info, "RDSEED", netp::CPUID::RDSEED());
		support_message(arch_info, "RDTSCP", netp::CPUID::RDTSCP());
		support_message(arch_info, "RTM", netp::CPUID::RTM());
		support_message(arch_info, "SEP", netp::CPUID::SEP());
		support_message(arch_info, "SHA", netp::CPUID::SHA());
		support_message(arch_info, "SSE", netp::CPUID::SSE());
		support_message(arch_info, "SSE2", netp::CPUID::SSE2());
		support_message(arch_info, "SSE3", netp::CPUID::SSE3());
		support_message(arch_info, "SSSE3", netp::CPUID::SSSE3());
		support_message(arch_info, "SSE4.1", netp::CPUID::SSE41());
		support_message(arch_info, "SSE4.2", netp::CPUID::SSE42());
		support_message(arch_info, "SSE4a", netp::CPUID::SSE4a());
		support_message(arch_info, "SYSCALL", netp::CPUID::SYSCALL());
		support_message(arch_info, "TBM", netp::CPUID::TBM());
		support_message(arch_info, "XOP", netp::CPUID::XOP());
		support_message(arch_info, "XSAVE", netp::CPUID::XSAVE());

		support_message(arch_info, "NEON", netp::CPUID::NEON());

		NETP_INFO("ARCH INFO: %s\n", arch_info.c_str());
		NETP_INFO("alignof(std::max_align_t): %d\n", alignof(std::max_align_t) );
	}

#if defined(NETP_DEBUG_OBJECT_SIZE)
	void app::__dump_sizeof() {

		NETP_INFO("sizeof(void*): %u", sizeof(void*));

		NETP_INFO("sizeof(std::atomic<long>): %u", sizeof(std::atomic<long>));
		NETP_INFO("sizeof(netp::__atomic_counter): %u", sizeof(netp::__atomic_counter));
		NETP_INFO("sizeof(netp::__non_atomic_counter): %u", sizeof(netp::__non_atomic_counter));
		NETP_INFO("sizeof(netp::ref_base): %u", sizeof(netp::ref_base));
		NETP_INFO("sizeof(netp::non_atomic_ref_base): %u", sizeof(netp::non_atomic_ref_base));
		NETP_INFO("sizeof(netp::packet): %u", sizeof(netp::packet));
		NETP_INFO("sizeof(ref_ptr<netp::packet>): %u", sizeof(NRP<netp::packet>));

		NETP_INFO("sizeof(address): %u", sizeof(netp::address));

		NETP_INFO("sizeof(netp::channel): %u", sizeof(netp::channel));
		NETP_INFO("sizeof(netp::io_ctx): %u", sizeof(netp::io_ctx));
		NETP_INFO("sizeof(netp::fn_io_event_t): %u", sizeof(netp::fn_io_event_t));
		NETP_INFO("sizeof(std::function<void(int)>): %u", sizeof(std::function<void(int)>));
		NETP_INFO("sizeof(std::function<void(int, int)>): %u", sizeof(std::function<void(int, int)>));
		NETP_INFO("sizeof(std::deque<socket_outbound_entry, netp::allocator<socket_outbound_entry>>): %u", sizeof(std::deque<socket_outbound_entry, netp::allocator<socket_outbound_entry>>));
		NETP_INFO("sizeof(netp::socket): %u", sizeof(netp::socket_channel));
		NETP_INFO("sizeof(std::vector<int>): %u", sizeof(std::vector<int>));
		NETP_INFO("sizeof(std::vector<std::function<void(int)>): %u", sizeof(std::vector<std::function<void(int)>>));
		NETP_INFO("sizeof(std::vector<std::function<void(int, int)>>): %u", sizeof(std::vector<std::function<void(int, int)>>));
		NETP_INFO("sizeof(netp::promise<int>): %u", sizeof(netp::promise<int>));
		NETP_INFO("sizeof(netp::promise<tuple<int,NRP<socket>>>): %u", sizeof(netp::promise<std::tuple<int, NRP<netp::socket_channel>>>));
		NETP_INFO("sizeof(netp::event_broker_promise): %u", sizeof(netp::event_broker_promise<int>));
		NETP_INFO("sizeof(netp::spin_mutex): %u", sizeof(netp::spin_mutex));
		NETP_INFO("sizeof(netp::mutex): %u", sizeof(netp::mutex));
		NETP_INFO("sizeof(netp::condition): %u", sizeof(netp::condition));
		NETP_INFO("sizeof(std::condition_variable): %u", sizeof(std::condition_variable));
		NETP_INFO("sizeof(netp::condition_any): %u", sizeof(netp::condition_any));
		NETP_INFO("sizeof(std::condition_variable_any): %u", sizeof(std::condition_variable_any));
	}
#endif

	void app::__signal_init() {
		netp::unique_lock<netp::mutex> ulk(m_mutex);
		signal_broker_init();

#if  defined(_NETP_GNU_LINUX) || defined(_NETP_ANDROID) || defined(_NETP_APPLE)
		signal_register(SIGPIPE, nullptr);
#endif

		i64_t handle_id_int =signal_register(SIGINT, std::bind(&app::handle_signal, this, std::placeholders::_1));
		if (handle_id_int > 0) {
			m_signo_tuple_vec.push_back(std::make_tuple(SIGINT, handle_id_int));
		}

		i64_t handle_id_term = signal_register(SIGTERM, std::bind(&app::handle_signal, this, std::placeholders::_1));
		if (handle_id_term > 0) {
			m_signo_tuple_vec.push_back(std::make_tuple(SIGTERM, handle_id_term));
		}
	}

	void app::__signal_deinit() {
		netp::unique_lock<netp::mutex> ulk(m_mutex);
		while (m_signo_tuple_vec.size()) {
			std::tuple<int, i64_t>& pair = m_signo_tuple_vec.back();
			signal_unregister(std::get<0>(pair), std::get<1>(pair));
			m_signo_tuple_vec.pop_back();
		}
		signal_broker_deinit();
		NETP_INFO("deinit signal end");
	}

	void app::__net_init() {
		NETP_INFO("net init begin");
#ifdef _NETP_WIN
		netp::os::winsock_init();
#endif

		if (m_app_event_loop_init_prev) {
			m_app_event_loop_init_prev();
		}	
		___event_loop_init();
		if (m_app_event_loop_init_post) {
			m_app_event_loop_init_post();
		}

		NETP_INFO("net init end");
	}

	void app::__net_deinit() {
		NETP_INFO("net deinit begin");
		netp::io_event_loop_group::instance()->_notify_terminating_all();
		netp::dns_resolver::instance()->stop();
		if (m_app_event_loop_deinit_prev) {
			m_app_event_loop_deinit_prev();
		}
		___event_loop_wait();
		if (m_app_event_loop_deinit_post) {
			m_app_event_loop_deinit_post();
		}

#ifdef _NETP_WIN
		netp::os::winsock_deinit();
#endif
		NETP_INFO("net deinit end");
	}

	void app::___event_loop_init() {
		netp::io_event_loop_group::instance()->_init(m_cfg.poller_count, m_cfg.event_loop_cfgs);
		NETP_INFO("[app]init loop done");

#if defined(NETP_DEFAULT_POLLER_TYPE_IOCP)
		io_event_loop_group::instance()->launch_loop(io_poller_type::T_SELECT, 1, m_cfg.event_loop_cfgs[NETP_DEFAULT_POLLER_TYPE]);
		netp::dns_resolver::instance()->reset(io_event_loop_group::instance()->internal_next(io_poller_type::T_SELECT));
#else
		netp::dns_resolver::instance()->reset(io_event_loop_group::instance()->internal_next());
#endif

#if defined(_NETP_WIN) && defined(_NETP_USE_UDNS)
		if (m_cfg.dnsnses.size() == 0) {
			vector_ipv4_t dnslist;
			netp::os::get_local_dns_server_list(dnslist);
			for (std::size_t i = 0; i < dnslist.size(); ++i) {
				NETP_INFO("[app]add dns: %s", netp::ipv4todotip(dnslist[i]).c_str());
				m_cfg.dnsnses.push_back( std::string(netp::ipv4todotip(dnslist[i]).c_str()));
			}
		}

		if (m_cfg.dnsnses.size() == 0) {
			NETP_ERR("[app]no dns nameserver");
		}
#endif
		
		if (m_cfg.dnsnses.size()) {
			netp::dns_resolver::instance()->add_name_server(m_cfg.dnsnses);
		}

		NRP<netp::promise<int>> dnsp = netp::dns_resolver::instance()->start();
		if (dnsp->get() != netp::OK) {
			NETP_ERR("[app]start dnsresolver failed: %d, exit", dnsp->get());
			_exit();
			exit(dnsp->get());
		}
		
		NETP_INFO("[app]init dns done");
	}

	void app::___event_loop_wait() {
		//reset loop after all loop reference dattached from business code
		netp::io_event_loop_group::instance()->_wait_all();
		netp::dns_resolver::instance()->reset(nullptr);
		netp::dns_resolver::destroy_instance();
	}

	//ISSUE: if the waken thread is main thread, we would get stuck here
	void app::handle_signal(int signo) {
		netp::unique_lock<netp::mutex> ulk(m_mutex);
		//NETP_ASSERT(false);
		NETP_INFO("[APP]receive signo: %d", signo);
		m_cond.notify_one();

#if defined(_NETP_GNU_LINUX) || defined(_NETP_ANDROID) || defined(_NETP_APPLE)
		if (signo == SIGPIPE) {
			return;//IGN
		}
#endif

		switch (signo) {
		case SIGINT:
		case SIGTERM:
		{
			m_should_exit = true;
		}
		break;
		}
	}

	bool app::should_exit() const {
		return m_should_exit;
	}

	void app_test_unit::test_memory() {	
		NRP<packet>* p = netp::allocator<NRP<packet>>::make();
		netp::allocator<NRP<packet>>::trash(p);

		NRP<packet>* pn = netp::allocator<NRP<packet>>::make_array(10);
		netp::allocator<NRP<packet>>::trash_array(pn,10);

		u8_t* u8ptr = netp::allocator<u8_t>::malloc(14, 16);
		netp::allocator<u8_t>::free(u8ptr);
	}

	void app_test_unit::test_packet() {

		{
			NRP<netp::packet> p = netp::make_ref<netp::packet>();
			for (size_t i = 0; i <= 1024*1024*2; ++i) {
				p->write<u8_t>(u8_t(1));
			}

			NRP<netp::packet> p2 = netp::make_ref<netp::packet>();
			for (size_t i = 0; i <= 1024*1024*2; ++i) {
				p2->write_left<u8_t>(u8_t(1));
			}

			NETP_ASSERT(*p == *p2);
		}

		NRP<netp::packet> tmp = netp::make_ref<netp::packet>(1024 * 1024);
	}

#ifdef _NETP_DEBUG
	#define POOL_CACHE_MAX_SIZE (16*1024)
#else
	#define POOL_CACHE_MAX_SIZE (64*1024)
#endif

	template <class vec_T, size_t size_max>
	void test_vector_pushback(size_t loop) {
		for (size_t k = 0; k < loop; ++k) {
			vec_T vector_int;
			for (size_t i = 0; i < size_max; ++i) {
				//NETP_INFO("vec.capacity(): %u", vector_int.capacity());
				vector_int.push_back(i);
			}
		}
	}

	struct __nonpod :
		public netp::non_atomic_ref_base
	{};

	template <class vec_T, size_t size_max>
	void test_vector_pushback_nonpod(size_t loop) {
		for (size_t k = 0; k < loop; ++k) {
			vec_T vector_int;
			for (size_t i = 0; i < size_max; ++i) {
				//NETP_INFO("vec.capacity(): %u", vector_int.capacity());
				vector_int.push_back(netp::make_ref<__nonpod>());
			}
		}
	}

	void app_test_unit::test_netp_allocator(size_t loop) {
		{
			netp::benchmark mk("std::vector<size_t,netp::allocator<size_t>");
			test_vector_pushback<std::vector<size_t,netp::allocator<size_t>>, POOL_CACHE_MAX_SIZE>(loop);
		}
		{
			netp::benchmark mk("std::vector<size_t,netp::allocator<size_t>");
			test_vector_pushback<std::vector<size_t,netp::allocator<size_t>>, POOL_CACHE_MAX_SIZE>(loop);
		}

		{
			netp::benchmark mk("std::vector<NRP<__nonpod>,netp::allocator<NRP<__nonpod>>");
			test_vector_pushback_nonpod<std::vector<NRP<__nonpod>, netp::allocator<NRP<__nonpod>>>, POOL_CACHE_MAX_SIZE>(loop);
		}
		{
			netp::benchmark mk("std::vector<NRP<__nonpod>,netp::allocator<NRP<__nonpod>>");
			test_vector_pushback_nonpod<std::vector<NRP<__nonpod>, netp::allocator<NRP<__nonpod>>>, POOL_CACHE_MAX_SIZE>(loop);
		}
	}

	void app_test_unit::test_std_allocator(size_t loop) {
		{
			netp::benchmark mk("std::vector<size_t,std::allocator<size_t>");
			test_vector_pushback<std::vector<size_t, std::allocator<size_t>>, POOL_CACHE_MAX_SIZE>(loop);
		}
		{
			netp::benchmark mk("std::vector<size_t,std::allocator<size_t>");
			test_vector_pushback<std::vector<size_t, std::allocator<size_t>>, POOL_CACHE_MAX_SIZE>(loop);
		}

		{
			netp::benchmark mk("std::vector<NRP<__nonpod>,std::allocator<NRP<__nonpod>>");
			test_vector_pushback_nonpod<std::vector<NRP<__nonpod>, std::allocator<NRP<__nonpod>>>, POOL_CACHE_MAX_SIZE>(loop);
		}
		{
			netp::benchmark mk("std::vector<NRP<__nonpod>,std::allocator<NRP<__nonpod>>");
			test_vector_pushback_nonpod<std::vector<NRP<__nonpod>, std::allocator<NRP<__nonpod>>>, POOL_CACHE_MAX_SIZE>(loop);
		}
	}

	void app_test_unit::test_generic_check() {
		const char* loopback[] = {
			"127.0.0.1",
			"10.0.0.0",
			"172.16.0.1",
			"192.168.0.0"
		};

		for (int i = 0; i < sizeof(loopback) / sizeof(loopback[0]); ++i) {
			ipv4_t v4 = netp::dotiptonip(loopback[i]);
			NETP_ASSERT(is_internal(v4));
		}
	}

	void app_test_unit::benchmark_hash() {
		int total = 100000;
		NRP<netp::packet> p = netp::make_ref<netp::packet>();
		for (int j = 0; j < 1500; ++j) {
			p->write<byte_t>(j%10);
		}
		{
			netp::benchmark mk("f16");
			for (int i = 0; i < total; ++i) {
				volatile u16_t h1 = netp::security::fletcher16((const uint8_t*)p->head(), p->len());
			}
		}
		{
			netp::benchmark mk("crc16");
			for (int i = 0; i < total; ++i) {
				volatile u16_t h2 = netp::security::crc16(p->head(), p->len());
			}
		}

		{
			NETP_INFO("crc16: %u", netp::security::crc16(p->head(), p->len()));
		}
	}

	bool app_test_unit::run() {
#ifdef _NETP_DEBUG
		test_memory();
#endif

		test_packet();

		size_t loop = 2;
#ifdef _NETP_DEBUG
		loop = 1;
#endif

		test_netp_allocator(loop);
		test_std_allocator(loop);

#ifdef _NETP_DEBUG
		test_generic_check();
		benchmark_hash();
#endif

		return true;
	}
}