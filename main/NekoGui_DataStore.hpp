// DO NOT INCLUDE THIS

namespace NekoGui {

    class Routing : public JsonStore {
    public:
        QString direct_ip;
        QString direct_domain;
        QString proxy_ip;
        QString proxy_domain;
        QString block_ip;
        QString block_domain;
        QString def_outbound = "proxy";
        QString custom = "{\"rules\": []}";

        // DNS
        QString remote_dns = "https://dns.google/dns-query";
        QString remote_dns_strategy = "";
        QString direct_dns = "https://doh.pub/dns-query";
        QString direct_dns_strategy = "";
        bool dns_routing = true;
        bool use_dns_object = false;
        QString dns_object = "";
        QString dns_final_out = "proxy";

        // Misc
        QString domain_strategy = "AsIs";
        QString outbound_domain_strategy = "AsIs";
        int sniffing_mode = SniffingMode::FOR_ROUTING;

        explicit Routing(int preset = 0);

        [[nodiscard]] QString DisplayRouting() const;

        static QStringList List();

        [[nodiscard]] static bool IsSafeName(const QString& name);

        static bool SetToActive(const QString& name);
    };

    class InboundAuthorization : public JsonStore {
    public:
        QString username;
        QString password;

        InboundAuthorization();

        [[nodiscard]] bool NeedAuth() const;
    };

    class DataStore : public JsonStore {
    public:
        // Running

        QString core_token;
        int core_port = 19810;
        int started_id = -1919;
        std::atomic_int core_transition_depth = 0;
        QMap<int, int> aux_profile_ports = {};
        // Written/read across the dedicated CoreProcess, UI and RPC worker
        // threads. These are observations/gates only; lifecycle ownership
        // remains in RuntimeTransition and the core lifecycle state machine.
        std::atomic_bool core_running = false;
        std::atomic_bool prepare_exit = false;
        bool spmode_vpn = false;
        bool spmode_system_proxy = false;
        bool need_keep_vpn_off = false;
        QString appdataDir = "";
        QStringList ignoreConnTag = {};

        std::unique_ptr<Routing> routing;
        int imported_count = 0;
        bool refreshing_group_list = false;
        bool refreshing_group = false;

        // Flags
        QStringList argv = {};
        bool flag_use_appdata = false;
        bool flag_many = false;
        bool flag_tray = false;
        bool flag_debug = false;
        int flag_restart_profile_id = -1919;
        bool flag_reorder = false;

        // Saved

        // Misc
        QString log_level = "info";
        QString test_latency_url = "http://cp.cloudflare.com/";
        QString test_download_url = "http://cachefly.cachefly.net/10mb.test";
        int test_download_timeout = 30;
        int test_concurrent = 5;
        int traffic_loop_interval = 1000;
        bool connection_statistics = false;
        int current_group = 0; // group id
        QString mux_protocol = "h2mux";
        bool mux_padding = false;
        int mux_concurrency = 8;
        bool mux_default_on = false;
        QString theme = "0";
        int language = 0;
        QString mw_size = "";
        bool check_include_pre = false;
        QString system_proxy_format = "";
        QStringList log_ignore = {};
        bool start_minimal = false;
        int max_log_line = 200;
        QString splitter_state = "";

        // Subscription
        QString user_agent = ""; // set at main.cpp
        bool sub_use_proxy = false;
        bool sub_clear = false;
        bool sub_insecure = false;
        int sub_auto_update = -30;

        // Security
        bool skip_cert = false;
        QString utlsFingerprint = "";

        // Remember
        QStringList remember_spmode = {};
        int remember_id = -1919;
        bool remember_enable = false;

        // Socks & HTTP Inbound
        QString inbound_address = "127.0.0.1";
        int inbound_socks_port = 12080; // or Mixed
        int aux_port_pool_start = 12100;
        int aux_port_pool_end = 12299;
        QStringList aux_profile_port_entries = {};
        QString aux_profile_ports_load_error = "";
        InboundAuthorization* inbound_auth = new InboundAuthorization;
        QString custom_inbound = "{\"inbounds\": []}";

        // Routing
        QString custom_route_global = "{\"rules\": []}";
        QString active_routing = "Default";

        // VPN
        bool fake_dns = false;
        bool vpn_internal_tun = true;
        int vpn_implementation = 0;
        int vpn_mtu = 9000;
        bool vpn_ipv6 = true;
        bool vpn_hide_console = true;
        bool vpn_strict_route = true;
        bool vpn_rule_white = false;
        QString vpn_rule_process = "";
        QString vpn_rule_cidr = "";

        // Hotkey
        QString hotkey_mainwindow = "";
        QString hotkey_group = "";
        QString hotkey_route = "";
        QString hotkey_system_proxy_menu = "";

        // Core
        int core_box_clash_api = -9090;
        QString core_box_clash_api_secret = "";
        QString core_box_underlying_dns = "";

        // Methods

        DataStore();

        void UpdateStartedId(int id);

        void LoadAuxiliaryProfilePorts();

        [[nodiscard]] QString ValidateAuxiliaryProfilePortEntries(const QJsonObject& object) const;

        [[nodiscard]] QString StoreAuxiliaryProfilePorts();

        void NormalizeAuxiliaryPortSettings();

        QString GetUserAgent(bool isDefault = false) const;
    };

    extern DataStore* dataStore;

} // namespace NekoGui
