// libraft - Quorum-based replication of states across machines.
// Copyright (c) 2015 Baidu.com, Inc. All Rights Reserved

// Author: WangYao (fisherman), wangyao02@baidu.com
// Date: 2015/10/08 17:00:05

#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <butil/logging.h>
#include <butil/files/file_path.h>
#include <butil/file_util.h>
#include <butil/fast_rand.h>
#include <brpc/closure_guard.h>
#include <bthread/bthread.h>
#include <bthread/countdown_event.h>
#include "braft/node.h"
#include "braft/enum.pb.h"
#include "braft/errno.pb.h"

namespace braft {
extern bvar::Adder<int64_t> g_num_nodes;
}
using braft::raft_mutex_t;

bool g_dont_print_apply_log = false;
class MockFSM : public braft::StateMachine {
public:
    MockFSM(const butil::EndPoint& address_)
        : address(address_)
        , applied_index(0)
        , snapshot_index(0)
        , _on_start_following_times(0)
        , _on_stop_following_times(0)
        , _leader_term(-1)
    {
        pthread_mutex_init(&mutex, NULL);
    }
    virtual ~MockFSM() {
        pthread_mutex_destroy(&mutex);
    }

    butil::EndPoint address;
    std::vector<butil::IOBuf> logs;
    pthread_mutex_t mutex;
    int64_t applied_index;
    int64_t snapshot_index;
    int64_t _on_start_following_times;
    int64_t _on_stop_following_times;
    volatile int64_t _leader_term;

    void lock() {
        pthread_mutex_lock(&mutex);
    }

    void on_leader_start(int64_t term) {
        _leader_term = term;
    }
    void on_leader_stop(const butil::Status&) {
        _leader_term = -1;
    }

    bool is_leader() { return _leader_term > 0; }

    void unlock() {
        pthread_mutex_unlock(&mutex);
    }

    virtual void on_apply(braft::Iterator& iter) {
        for (; iter.valid(); iter.next()) {
            LOG_IF(INFO, !g_dont_print_apply_log) << "addr " << address << " apply " << iter.index();
            ::brpc::ClosureGuard guard(iter.done());
            lock();
            logs.push_back(iter.data());
            unlock();
            applied_index = iter.index();
        }
    }

    virtual void on_shutdown() {
        LOG(INFO) << "addr " << address << " shutdowned";
    }

    virtual void on_snapshot_save(braft::SnapshotWriter* writer, braft::Closure* done) {
        std::string file_path = writer->get_path();
        file_path.append("/data");
        brpc::ClosureGuard done_guard(done);

        LOG(INFO) << "on_snapshot_save to " << file_path;

        int fd = ::creat(file_path.c_str(), 0644);
        if (fd < 0) {
            LOG(ERROR) << "create file failed, path: " << file_path << " err: " << berror();
            done->status().set_error(EIO, "Fail to create file");
            return;
        }
        lock();
        // write snapshot and log to file
        for (size_t i = 0; i < logs.size(); i++) {
            butil::IOBuf data = logs[i];
            int len = data.size();
            int ret = write(fd, &len, sizeof(int));
            CHECK_EQ(ret, 4);
            data.cut_into_file_descriptor(fd, len);
        }
        ::close(fd);
        snapshot_index = applied_index;
        unlock();
        writer->add_file("data");
    }

    virtual int on_snapshot_load(braft::SnapshotReader* reader) {
        std::string file_path = reader->get_path();
        file_path.append("/data");

        LOG(INFO) << "on_snapshot_load from " << file_path;

        int fd = ::open(file_path.c_str(), O_RDONLY);
        if (fd < 0) {
            LOG(ERROR) << "creat file failed, path: " << file_path << " err: " << berror();
            return EIO;
        }

        lock();
        logs.clear();
        while (true) {
            int len = 0;
            int ret = read(fd, &len, sizeof(int));
            if (ret <= 0) {
                break;
            }

            butil::IOPortal data;
            data.append_from_file_descriptor(fd, len);
            logs.push_back(data);
        }

        ::close(fd);
        unlock();
        return 0;
    }

    virtual void on_start_following(const braft::LeaderChangeContext& start_following_context) {
        LOG(INFO) << "address " << address << " start following new leader: " 
                   <<  start_following_context;
        ++_on_start_following_times;
    }

    virtual void on_stop_following(const braft::LeaderChangeContext& stop_following_context) {
        LOG(INFO) << "address " << address << " stop following old leader: " 
                   <<  stop_following_context;
        ++_on_stop_following_times;
    }

    virtual void on_configuration_committed(const ::braft::Configuration& conf) {
        LOG(INFO) << "address " << address << " commit conf: " << conf;
    }

};

class ExpectClosure : public braft::Closure {
public:
    void Run() {
        if (_expect_err_code >= 0) {
            EXPECT_EQ(status().error_code(), _expect_err_code) 
                << _pos << " : " << status();
                                                
        }
        if (_cond) {
            _cond->signal();
        }
        delete this;
    }
private:
    ExpectClosure(bthread::CountdownEvent* cond, int expect_err_code, const char* pos)
        : _cond(cond), _expect_err_code(expect_err_code), _pos(pos) {}

    ExpectClosure(bthread::CountdownEvent* cond, const char* pos)
        : _cond(cond), _expect_err_code(-1), _pos(pos) {}

    bthread::CountdownEvent* _cond;
    int _expect_err_code;
    const char* _pos;
};

typedef ExpectClosure ShutdownClosure;
typedef ExpectClosure ApplyClosure;
typedef ExpectClosure AddPeerClosure;
typedef ExpectClosure RemovePeerClosure;
typedef ExpectClosure SnapshotClosure;

#define NEW_SHUTDOWNCLOSURE(arg...) \
        (new ExpectClosure(arg, __FILE__ ":" BAIDU_SYMBOLSTR(__LINE__)))
#define NEW_APPLYCLOSURE(arg...) \
        (new ExpectClosure(arg, __FILE__ ":" BAIDU_SYMBOLSTR(__LINE__)))
#define NEW_ADDPEERCLOSURE(arg...) \
        (new ExpectClosure(arg, __FILE__ ":" BAIDU_SYMBOLSTR(__LINE__)))
#define NEW_REMOVEPEERCLOSURE(arg...) \
        (new ExpectClosure(arg, __FILE__ ":" BAIDU_SYMBOLSTR(__LINE__)))
#define NEW_SNAPSHOTCLOSURE(arg...) \
        (new ExpectClosure(arg, __FILE__ ":" BAIDU_SYMBOLSTR(__LINE__)))

class Cluster {
public:
    Cluster(const std::string& name, const std::vector<braft::PeerId>& peers,
            int32_t election_timeout_ms = 300)
        : _name(name), _peers(peers) 
        , _election_timeout_ms(election_timeout_ms) {
    }
    ~Cluster() {
        stop_all();
    }

    int start(const butil::EndPoint& listen_addr, bool empty_peers = false,
              int snapshot_interval_s = 30) {
        if (_server_map[listen_addr] == NULL) {
            brpc::Server* server = new brpc::Server();
            if (braft::add_service(server, listen_addr) != 0 
                    || server->Start(listen_addr, NULL) != 0) {
                LOG(ERROR) << "Fail to start raft service";
                delete server;
                return -1;
            }
            _server_map[listen_addr] = server;
        }

        braft::NodeOptions options;
        options.election_timeout_ms = _election_timeout_ms;
        options.snapshot_interval_s = snapshot_interval_s;
        if (!empty_peers) {
            options.initial_conf = braft::Configuration(_peers);
        }
        MockFSM* fsm = new MockFSM(listen_addr);
        options.fsm = fsm;
        options.node_owns_fsm = true;
        butil::string_printf(&options.log_uri, "local://./data/%s/log",
                            butil::endpoint2str(listen_addr).c_str());
        butil::string_printf(&options.raft_meta_uri, "local://./data/%s/raft_meta",
                            butil::endpoint2str(listen_addr).c_str());
        butil::string_printf(&options.snapshot_uri, "local://./data/%s/snapshot",
                            butil::endpoint2str(listen_addr).c_str());

        braft::Node* node = new braft::Node(_name, braft::PeerId(listen_addr, 0));
        int ret = node->init(options);
        if (ret != 0) {
            LOG(WARNING) << "init_node failed, server: " << listen_addr;
            return ret;
        } else {
            LOG(INFO) << "init node " << listen_addr;
        }

        {
            std::lock_guard<raft_mutex_t> guard(_mutex);
            _nodes.push_back(node);
            _fsms.push_back(fsm);
        }
        return 0;
    }

    int stop(const butil::EndPoint& listen_addr) {
        
        bthread::CountdownEvent cond;
        braft::Node* node = remove_node(listen_addr);
        if (node) {
            node->shutdown(NEW_SHUTDOWNCLOSURE(&cond));
            cond.wait();
            node->join();
        }

        delete _server_map[listen_addr];
        _server_map.erase(listen_addr);
        delete node;
        return node ? 0 : -1;
    }

    void stop_all() {
        std::vector<butil::EndPoint> addrs;
        std::vector<braft::Node*> nodes;
        all_nodes(&addrs);
        for (auto addr : addrs) {
            braft::Node* node = remove_node(addr);
            node->shutdown(NULL);
            nodes.push_back(node);
            std::cout << "Stopping server " << addr << std::endl;
            delete _server_map[addr];
            _server_map.erase(addr);
        }
        for (auto node : nodes) {
            std::cout << "join node " << node->node_id() << std::endl;
            node->join();
            delete node;
        }
    }

    void clean(const butil::EndPoint& listen_addr) {
        std::string data_path;
        butil::string_printf(&data_path, "./data/%s",
                            butil::endpoint2str(listen_addr).c_str());

        if (!butil::DeleteFile(butil::FilePath(data_path), true)) {
            LOG(ERROR) << "delete path failed, path: " << data_path;
        }
    }

    braft::Node* leader() {
        std::lock_guard<raft_mutex_t> guard(_mutex);
        braft::Node* node = NULL;
        for (size_t i = 0; i < _nodes.size(); i++) {
            if (_nodes[i]->is_leader() &&
                    _fsms[i]->_leader_term == _nodes[i]->_impl->_current_term) {
                node = _nodes[i];
                break;
            }
        }
        return node;
    }

    void followers(std::vector<braft::Node*>* nodes) {
        nodes->clear();

        std::lock_guard<raft_mutex_t> guard(_mutex);
        for (size_t i = 0; i < _nodes.size(); i++) {
            if (!_nodes[i]->is_leader()) {
                nodes->push_back(_nodes[i]);
            }
        }
    }
    
    void wait_leader() {
        while (true) {
            braft::Node* node = leader();
            if (node) {
                return;
            } else {
                usleep(100 * 1000);
            }
        }
    }

    void ensure_leader(const butil::EndPoint& expect_addr) {
CHECK:
        std::lock_guard<raft_mutex_t> guard(_mutex);
        for (size_t i = 0; i < _nodes.size(); i++) {
            braft::PeerId leader_id = _nodes[i]->leader_id();
            if (leader_id.addr != expect_addr) {
                goto WAIT;
            }
        }

        return;
WAIT:
        usleep(100 * 1000);
        goto CHECK;
    }

    bool ensure_same(int wait_time_s = -1) {
        std::lock_guard<raft_mutex_t> guard(_mutex);
        if (_fsms.size() <= 1) {
            return true;
        }
        LOG(INFO) << "_fsms.size()=" << _fsms.size();

        int nround = 0;
        MockFSM* first = _fsms[0];
CHECK:
        first->lock();
        for (size_t i = 1; i < _fsms.size(); i++) {
            MockFSM* fsm = _fsms[i];
            fsm->lock();

            if (first->logs.size() != fsm->logs.size()) {
                fsm->unlock();
                goto WAIT;
            }

            for (size_t j = 0; j < first->logs.size(); j++) {
                butil::IOBuf& first_data = first->logs[j];
                butil::IOBuf& fsm_data = fsm->logs[j];
                if (first_data.to_string() != fsm_data.to_string()) {
                    fsm->unlock();
                    goto WAIT;
                }
            }

            fsm->unlock();
        }
        first->unlock();

        return true;
WAIT:
        first->unlock();
        sleep(1);
        ++nround;
        if (wait_time_s > 0 && nround > wait_time_s) {
            return false;
        }
        goto CHECK;
    }

private:
    void all_nodes(std::vector<butil::EndPoint>* addrs) {
        addrs->clear();

        std::lock_guard<raft_mutex_t> guard(_mutex);
        for (size_t i = 0; i < _nodes.size(); i++) {
            addrs->push_back(_nodes[i]->node_id().peer_id.addr);
        }
    }

    braft::Node* remove_node(const butil::EndPoint& addr) {
        std::lock_guard<raft_mutex_t> guard(_mutex);

        // remove node
        braft::Node* node = NULL;
        std::vector<braft::Node*> new_nodes;
        for (size_t i = 0; i < _nodes.size(); i++) {
            if (addr.port == _nodes[i]->node_id().peer_id.addr.port) {
                node = _nodes[i];
            } else {
                new_nodes.push_back(_nodes[i]);
            }
        }
        _nodes.swap(new_nodes);

        // remove fsm
        std::vector<MockFSM*> new_fsms;
        for (size_t i = 0; i < _fsms.size(); i++) {
            if (_fsms[i]->address != addr) {
                new_fsms.push_back(_fsms[i]);
            }
        }
        _fsms.swap(new_fsms);

        return node;
    }

    std::string _name;
    std::vector<braft::PeerId> _peers;
    std::vector<braft::Node*> _nodes;
    std::vector<MockFSM*> _fsms;
    std::map<butil::EndPoint, brpc::Server*> _server_map;
    int32_t _election_timeout_ms;
    raft_mutex_t _mutex;
};

class NodeTest : public testing::Test {
protected:
    void SetUp() {
        g_dont_print_apply_log = false;
        //logging::FLAGS_verbose = 90;
        ::system("rm -rf data");
        ASSERT_EQ(0, braft::g_num_nodes.get_value());
    }
    void TearDown() {
        ::system("rm -rf data");
        // Sleep for a while to wait all timer has stopped
        if (braft::g_num_nodes.get_value() != 0) {
            usleep(1000 * 1000);
            ASSERT_EQ(0, braft::g_num_nodes.get_value());
        }
    }
};

TEST_F(NodeTest, InitShutdown) {
    brpc::Server server;
    int ret = braft::add_service(&server, "0.0.0.0:5006");
    ASSERT_EQ(0, ret);
    ASSERT_EQ(0, server.Start("0.0.0.0:5006", NULL));

    braft::NodeOptions options;
    options.fsm = new MockFSM(butil::EndPoint());
    options.log_uri = "local://./data/log";
    options.raft_meta_uri = "local://./data/raft_meta";
    options.snapshot_uri = "local://./data/snapshot";

    braft::Node node("unittest", braft::PeerId(butil::EndPoint(butil::my_ip(), 5006), 0));
    ASSERT_EQ(0, node.init(options));

    node.shutdown(NULL);
    node.join();

    //FIXME:
    bthread::CountdownEvent cond;
    butil::IOBuf data;
    data.append("hello");
    braft::Task task;
    task.data = &data;
    task.done = NEW_APPLYCLOSURE(&cond);
    node.apply(task);
    cond.wait();
}

TEST_F(NodeTest, Server) {
    brpc::Server server1;
    brpc::Server server2;
    ASSERT_EQ(0, braft::add_service(&server1, "0.0.0.0:5006"));
    ASSERT_EQ(0, braft::add_service(&server1, "0.0.0.0:5006"));
    ASSERT_EQ(0, braft::add_service(&server2, "0.0.0.0:5007"));
    server1.Start("0.0.0.0:5006", NULL);
    server2.Start("0.0.0.0:5007", NULL);
}

TEST_F(NodeTest, SingleNode) {
    brpc::Server server;
    int ret = braft::add_service(&server, 5006);
    server.Start(5006, NULL);
    ASSERT_EQ(0, ret);

    braft::PeerId peer;
    peer.addr.ip = butil::my_ip();
    peer.addr.port = 5006;
    peer.idx = 0;
    std::vector<braft::PeerId> peers;
    peers.push_back(peer);

    braft::NodeOptions options;
    options.election_timeout_ms = 300;
    options.initial_conf = braft::Configuration(peers);
    options.fsm = new MockFSM(butil::EndPoint());
    options.log_uri = "local://./data/log";
    options.raft_meta_uri = "local://./data/raft_meta";
    options.snapshot_uri = "local://./data/snapshot";

    braft::Node node("unittest", peer);
    ASSERT_EQ(0, node.init(options));

    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        node.apply(task);
    }
    cond.wait();

    cond.reset(1);
    node.shutdown(NEW_SHUTDOWNCLOSURE(&cond, 0));
    cond.wait();

    server.Stop(200);
    server.Join();
}

TEST_F(NodeTest, NoLeader) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    cluster.start(peers[0].addr);

    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(1, nodes.size());

    braft::Node* follower = nodes[0];

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, EPERM);
        follower->apply(task);
    }
    cond.wait();

    // add peer1
    braft::PeerId peer3;
    peer3.addr.ip = butil::my_ip();
    peer3.addr.port = 5006 + 3;
    peer3.idx = 0;

    cond.reset(1);
    follower->add_peer(peer3, NEW_ADDPEERCLOSURE(&cond, EPERM));
    cond.wait();
    LOG(INFO) << "add peer " << peer3;

    // remove peer1
    braft::PeerId peer0;
    peer0.addr.ip = butil::my_ip();
    peer0.addr.port = 5006 + 0;
    peer0.idx = 0;

    cond.reset(1);
    follower->remove_peer(peer0, NEW_REMOVEPEERCLOSURE(&cond, EPERM));
    cond.wait();
    LOG(INFO) << "remove peer " << peer0;
}

TEST_F(NodeTest, TripleNode) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "no closure");
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        leader->apply(task);
    }

    cluster.ensure_same();

    {
        brpc::Channel channel;
        brpc::ChannelOptions options;
        options.protocol = brpc::PROTOCOL_HTTP;

        if (channel.Init(leader->node_id().peer_id.addr, &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel";
        }

        {
            brpc::Controller cntl;
            cntl.http_request().uri() = "/raft_stat";
            cntl.http_request().set_method(brpc::HTTP_METHOD_GET);

            channel.CallMethod(NULL, &cntl, NULL, NULL, NULL/*done*/);

            LOG(INFO) << "http return: \n" << cntl.response_attachment();
        }

        {
            brpc::Controller cntl;
            cntl.http_request().uri() = "/raft_stat/unittest";
            cntl.http_request().set_method(brpc::HTTP_METHOD_GET);

            channel.CallMethod(NULL, &cntl, NULL, NULL, NULL/*done*/);

            LOG(INFO) << "http return: \n" << cntl.response_attachment();
        }
    }

    // stop cluster
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());

    LOG(WARNING) << "cluster stop";
    cluster.stop_all();
}

TEST_F(NodeTest, LeaderFail) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // stop leader
    butil::EndPoint old_leader = leader->node_id().peer_id.addr;
    LOG(WARNING) << "stop leader " << leader->node_id();
    cluster.stop(leader->node_id().peer_id.addr);

    // apply something when follower
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    cond.reset(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "follower apply: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, -1);
        nodes[0]->apply(task);
    }
    cond.wait();

    // elect new leader
    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "elect new leader " << leader->node_id();

    // apply something
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // old leader restart
    ASSERT_EQ(0, cluster.start(old_leader));
    LOG(WARNING) << "restart old leader " << old_leader;

    // apply something
    cond.reset(10);
    for (int i = 20; i < 30; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // stop and clean old leader
    LOG(WARNING) << "stop old leader " << old_leader;
    cluster.stop(old_leader);
    LOG(WARNING) << "clean old leader data " << old_leader;
    cluster.clean(old_leader);

    sleep(2);
    // restart old leader
    ASSERT_EQ(0, cluster.start(old_leader));
    LOG(WARNING) << "restart old leader " << old_leader;

    cluster.ensure_same();

    cluster.stop_all();
}

TEST_F(NodeTest, JoinNode) {
    std::vector<braft::PeerId> peers;
    braft::PeerId peer0;
    peer0.addr.ip = butil::my_ip();
    peer0.addr.port = 5006;
    peer0.idx = 0;

    // start cluster
    peers.push_back(peer0);
    Cluster cluster("unittest", peers);
    ASSERT_EQ(0, cluster.start(peer0.addr));
    LOG(INFO) << "start single cluster " << peer0;

    cluster.wait_leader();

    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    ASSERT_EQ(leader->node_id().peer_id, peer0);
    LOG(WARNING) << "leader is " << leader->node_id();

    bthread::CountdownEvent cond(10);
    // apply something
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // start peer1
    braft::PeerId peer1;
    peer1.addr.ip = butil::my_ip();
    peer1.addr.port = 5006 + 1;
    peer1.idx = 0;
    ASSERT_EQ(0, cluster.start(peer1.addr, true));
    LOG(INFO) << "start peer " << peer1;
    // wait until started successfully
    usleep(1000* 1000);

    // add peer1
    cond.reset(1);
    leader->add_peer(peer1, NEW_ADDPEERCLOSURE(&cond, 0));
    cond.wait();
    LOG(INFO) << "add peer " << peer1;

    cluster.ensure_same();

    // add peer2 when peer not start
    braft::PeerId peer2;
    peer2.addr.ip = butil::my_ip();
    peer2.addr.port = 5006 + 2;
    peer2.idx = 0;

    cond.reset(1);
    peers.push_back(peer1);
    leader->add_peer(peer2, NEW_ADDPEERCLOSURE(&cond, braft::ECATCHUP));
    cond.wait();

    // start peer2 after some seconds wait 
    sleep(2);
    ASSERT_EQ(0, cluster.start(peer2.addr, true));
    LOG(INFO) << "start peer " << peer2;

    usleep(1000 * 1000L);

    braft::PeerId peer4("192.168.1.1:1234");

    // re add peer2
    cond.reset(2);
    // {peer0,peer1} add peer2
    leader->add_peer(peer2, NEW_ADDPEERCLOSURE(&cond, 0));
    // concurrent configration change
    leader->add_peer(peer4, NEW_ADDPEERCLOSURE(&cond, EBUSY));
    cond.wait();

    cond.reset(1);
    // retry add_peer direct ok
    leader->add_peer(peer2, NEW_ADDPEERCLOSURE(&cond, 0));
    cond.wait();

    cluster.ensure_same();

    cluster.stop_all();
}

TEST_F(NodeTest, RemoveFollower) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    bthread::CountdownEvent cond(10);
    // apply something
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    cluster.ensure_same();

    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());

    const braft::PeerId follower_id = nodes[0]->node_id().peer_id;
    const butil::EndPoint follower_addr = follower_id.addr;
    // stop follower
    LOG(WARNING) << "stop and clean follower " << follower_addr;
    cluster.stop(follower_addr);
    cluster.clean(follower_addr);

    // remove follower
    LOG(WARNING) << "remove follower " << follower_addr;
    cond.reset(1);
    leader->remove_peer(follower_id, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();

    // apply something
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    cluster.followers(&nodes);
    ASSERT_EQ(1, nodes.size());

    peers.clear();
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        if (peer.addr != follower_addr) {
            peers.push_back(peer);
        }
    }

    // start follower
    LOG(WARNING) << "start follower " << follower_addr;
    ASSERT_EQ(0, cluster.start(follower_addr));

    // re add follower fail when leader step down
    LOG(WARNING) << "add follower " << follower_addr;
    cond.reset(1);
    leader->add_peer(follower_id, NEW_ADDPEERCLOSURE(&cond, 0));
    cond.wait();

    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());

    cluster.ensure_same();
}

TEST_F(NodeTest, RemoveLeader) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    bthread::CountdownEvent cond(10);
    // apply something
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    butil::EndPoint old_leader_addr = leader->node_id().peer_id.addr;
    LOG(WARNING) << "remove leader " << old_leader_addr;
    cond.reset(1);
    leader->remove_peer(leader->node_id().peer_id, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();

    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    // apply something
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    LOG(INFO) << "here";
    cond.wait();

    LOG(WARNING) << "stop and clear leader " << old_leader_addr;
    cluster.stop(old_leader_addr);
    cluster.clean(old_leader_addr);

    LOG(WARNING) << "start old leader " << old_leader_addr;
    cluster.start(old_leader_addr);

    LOG(WARNING) << "add old leader " << old_leader_addr;
    cond.reset(1);
    peers.clear();
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        if (peer.addr != old_leader_addr) {
            peers.push_back(peer);
        }
    }
    leader->add_peer(braft::PeerId(old_leader_addr, 0), NEW_ADDPEERCLOSURE(&cond, 0));
    cond.wait();

    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());

    cluster.ensure_same();
}

TEST_F(NodeTest, PreVote) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    bthread::CountdownEvent cond(10);
    // apply something
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    cluster.ensure_same();

    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());
    const braft::PeerId follower_id = nodes[0]->node_id().peer_id;
    const butil::EndPoint follower_addr = follower_id.addr;

    const int64_t saved_term = leader->_impl->_current_term;
    //remove follower
    LOG(WARNING) << "remove follower " << follower_addr;
    cond.reset(1);
    leader->remove_peer(follower_id, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();

    // apply something
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    sleep(2);

    //add follower
    LOG(WARNING) << "add follower " << follower_addr;
    peers.clear();
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        if (peer.addr != follower_addr) {
            peers.push_back(peer);
        }
    }
    cond.reset(1);
    leader->add_peer(follower_id, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();

    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);

    ASSERT_EQ(saved_term, leader->_impl->_current_term);
}

TEST_F(NodeTest, SetPeer1) {
    // bootstrap from null
    Cluster cluster("unittest", std::vector<braft::PeerId>());
    braft::PeerId boot_peer;
    boot_peer.addr.ip = butil::my_ip();
    boot_peer.addr.port = 5006;
    boot_peer.idx = 0;

    ASSERT_EQ(0, cluster.start(boot_peer.addr));
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(1, nodes.size());

    std::vector<braft::PeerId> peers;
    peers.push_back(boot_peer);
    ASSERT_TRUE(nodes[0]->reset_peers(braft::Configuration(peers)).ok());

    cluster.wait_leader();
}

TEST_F(NodeTest, SetPeer2) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    std::cout << "Here" << std::endl;
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    butil::EndPoint leader_addr = leader->node_id().peer_id.addr;
    LOG(WARNING) << "leader is " << leader->node_id();
    std::cout << "Here" << std::endl;

    bthread::CountdownEvent cond(10);
    // apply something
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    std::cout << "Here" << std::endl;

    // check follower
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());
    braft::PeerId follower_peer1 = nodes[0]->node_id().peer_id;
    braft::PeerId follower_peer2 = nodes[1]->node_id().peer_id;

    LOG(WARNING) << "stop and clean follower " << follower_peer1;
    cluster.stop(follower_peer1.addr);
    cluster.clean(follower_peer1.addr);

    std::cout << "Here" << std::endl;
    // apply something
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    
    std::cout << "Here" << std::endl;
    //set peer when no quorum die
    std::vector<braft::PeerId> new_peers;
    LOG(WARNING) << "set peer to " << leader_addr;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        if (peer != follower_peer1) {
            new_peers.push_back(peer);
        }
    }
    LOG(WARNING) << "stop and clean follower " << follower_peer2;
    cluster.stop(follower_peer2.addr);
    cluster.clean(follower_peer2.addr);

    // leader will stepdown, become follower
    sleep(2);

    new_peers.clear();
    new_peers.push_back(braft::PeerId(leader_addr, 0));

    // new peers equal current conf
    ASSERT_TRUE(leader->reset_peers(braft::Configuration(peers)).ok());
    // set peer when quorum die
    LOG(WARNING) << "set peer to " << leader_addr;
    new_peers.clear();
    new_peers.push_back(braft::PeerId(leader_addr, 0));
    ASSERT_TRUE(leader->reset_peers(braft::Configuration(new_peers)).ok());

    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    ASSERT_EQ(leader->node_id().peer_id.addr, leader_addr);

    LOG(WARNING) << "start old follower " << follower_peer1;
    ASSERT_EQ(0, cluster.start(follower_peer1.addr, true));
    LOG(WARNING) << "start old follower " << follower_peer2;
    ASSERT_EQ(0, cluster.start(follower_peer2.addr, true));

    LOG(WARNING) << "add old follower " << follower_peer1;
    cond.reset(1);
    leader->add_peer(follower_peer1, NEW_ADDPEERCLOSURE(&cond, 0));
    cond.wait();

    LOG(WARNING) << "add old follower " << follower_peer2;
    cond.reset(1);
    new_peers.push_back(follower_peer1);
    leader->add_peer(follower_peer2, NEW_ADDPEERCLOSURE(&cond, 0));
    cond.wait();

    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());

    cluster.ensure_same();
}

TEST_F(NodeTest, RestoreSnapshot) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();
    butil::EndPoint leader_addr = leader->node_id().peer_id.addr;

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    cluster.ensure_same();

    // trigger leader snapshot
    LOG(WARNING) << "trigger leader snapshot ";
    cond.reset(1);
    leader->snapshot(NEW_SNAPSHOTCLOSURE(&cond, 0));
    cond.wait();

    // stop leader
    LOG(WARNING) << "stop leader";
    cluster.stop(leader->node_id().peer_id.addr);

    sleep(2);

    LOG(WARNING) << "restart leader";
    ASSERT_EQ(0, cluster.start(leader_addr));

    cluster.ensure_same();

    LOG(WARNING) << "cluster stop";
    cluster.stop_all();
}

TEST_F(NodeTest, InstallSnapshot) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    cluster.ensure_same();

    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(2, nodes.size());

    // stop follower
    LOG(WARNING) << "stop follower";
    butil::EndPoint follower_addr = nodes[0]->node_id().peer_id.addr;
    cluster.stop(follower_addr);

    // apply something
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // trigger leader snapshot
    LOG(WARNING) << "trigger leader snapshot ";
    cond.reset(1);
    leader->snapshot(NEW_SNAPSHOTCLOSURE(&cond, 0));
    cond.wait();

    // apply something
    cond.reset(10);
    for (int i = 20; i < 30; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // wait leader to compact logs
    usleep(5000 * 1000);

    LOG(WARNING) << "restart follower";
    ASSERT_EQ(0, cluster.start(follower_addr));

    sleep(2);

    cluster.ensure_same();

    LOG(WARNING) << "cluster stop";
    cluster.stop_all();
}

TEST_F(NodeTest, NoSnapshot) {
    brpc::Server server;
    brpc::ServerOptions server_options;
    int ret = braft::add_service(&server, "0.0.0.0:5006");
    ASSERT_EQ(0, ret);
    ASSERT_EQ(0, server.Start(5006, &server_options));

    braft::PeerId peer;
    peer.addr.ip = butil::my_ip();
    peer.addr.port = 5006;
    peer.idx = 0;
    std::vector<braft::PeerId> peers;
    peers.push_back(peer);

    braft::NodeOptions options;
    options.election_timeout_ms = 300;
    options.initial_conf = braft::Configuration(peers);
    options.fsm = new MockFSM(butil::EndPoint());
    options.log_uri = "local://./data/log";
    options.raft_meta_uri = "local://./data/raft_meta";

    braft::Node node("unittest", peer);
    ASSERT_EQ(0, node.init(options));

    // wait node elect to leader
    sleep(2);

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        node.apply(task);
    }
    cond.wait();

    // trigger snapshot, not expect ret
    cond.reset(1);
    node.snapshot(NEW_SNAPSHOTCLOSURE(&cond, -1));
    cond.wait();

    // shutdown
    cond.reset(1);
    node.shutdown(NEW_SHUTDOWNCLOSURE(&cond, 0));
    cond.wait();

    // stop
    server.Stop(200);
    server.Join();
}

TEST_F(NodeTest, AutoSnapshot) {
    brpc::Server server;
    brpc::ServerOptions server_options;
    int ret = braft::add_service(&server, "0.0.0.0:5006");
    ASSERT_EQ(0, ret);
    ASSERT_EQ(0, server.Start(5006, &server_options));

    braft::PeerId peer;
    peer.addr.ip = butil::my_ip();
    peer.addr.port = 5006;
    peer.idx = 0;
    std::vector<braft::PeerId> peers;
    peers.push_back(peer);

    braft::NodeOptions options;
    options.election_timeout_ms = 300;
    options.initial_conf = braft::Configuration(peers);
    options.fsm = new MockFSM(butil::EndPoint());
    options.log_uri = "local://./data/log";
    options.raft_meta_uri = "local://./data/raft_meta";
    options.snapshot_uri = "local://./data/snapshot";
    options.snapshot_interval_s = 10;

    braft::Node node("unittest", peer);
    ASSERT_EQ(0, node.init(options));

    // wait node elect to leader
    sleep(2);

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        node.apply(task);
    }
    cond.wait();

    sleep(10);
    ASSERT_GT(static_cast<MockFSM*>(options.fsm)->snapshot_index, 0);

    // shutdown
    cond.reset(1);
    node.shutdown(NEW_SHUTDOWNCLOSURE(&cond, 0));
    cond.wait();

    // stop
    server.Stop(200);
    server.Join();
}

TEST_F(NodeTest, LeaderShouldNotChange) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader0 = cluster.leader();
    ASSERT_TRUE(leader0 != NULL);
    LOG(WARNING) << "leader is " << leader0->node_id();
    const int64_t saved_term = leader0->_impl->_current_term;
    usleep(5000 * 1000);
    cluster.wait_leader();
    braft::Node* leader1 = cluster.leader();
    LOG(WARNING) << "leader is " << leader1->node_id();
    ASSERT_EQ(leader0, leader1);
    ASSERT_EQ(saved_term, leader1->_impl->_current_term);
    cluster.stop_all();
}

TEST_F(NodeTest, RecoverFollower) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr, false, 1));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    usleep(1000 * 1000);
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_FALSE(nodes.empty());
    const butil::EndPoint follower_addr = nodes[0]->_impl->_server_id.addr;
    cluster.stop(follower_addr);

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);

        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "no closure");
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        leader->apply(task);
    }
    // wait leader to compact logs
    usleep(5000 * 1000);

    // Start the stopped follower, expecting that leader would recover it
    LOG(WARNING) << "restart follower";
    cluster.start(follower_addr);
    LOG(WARNING) << "restart follower done";
    LOG(WARNING) << "here";
    ASSERT_TRUE(cluster.ensure_same(5));
    LOG(WARNING) << "cluster stop";
    cluster.stop_all();
}

TEST_F(NodeTest, leader_transfer) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr, false, 1));
    }
    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    braft::PeerId target = nodes[0]->node_id().peer_id;
    ASSERT_EQ(0, leader->transfer_leadership_to(target));
    usleep(10 * 1000);
    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_EQ(target, leader->node_id().peer_id);
    ASSERT_TRUE(cluster.ensure_same(5));
    cluster.stop_all();
}

TEST_F(NodeTest, leader_transfer_before_log_is_compleleted) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers, 5000);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr, false, 1));
    }
    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    braft::PeerId target = nodes[0]->node_id().peer_id;
    cluster.stop(target.addr);
    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    ASSERT_EQ(0, leader->transfer_leadership_to(target));
    cond.reset(1);
    braft::Task task;
    butil::IOBuf data;
    data.resize(5, 'a');
    task.data = &data;
    task.done = NEW_APPLYCLOSURE(&cond, EBUSY);
    leader->apply(task);
    cond.wait();
    cluster.start(target.addr);
    usleep(5000 * 1000);
    LOG(INFO) << "here";
    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_EQ(target, leader->node_id().peer_id);
    ASSERT_TRUE(cluster.ensure_same(5));
    cluster.stop_all();
}

TEST_F(NodeTest, leader_transfer_resume_on_failure) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr, false, 1));
    }
    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    braft::PeerId target = nodes[0]->node_id().peer_id;
    cluster.stop(target.addr);
    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    ASSERT_EQ(0, leader->transfer_leadership_to(target));
    braft::Node* saved_leader = leader;
    cond.reset(1);
    braft::Task task;
    butil::IOBuf data;
    data.resize(5, 'a');
    task.data = &data;
    task.done = NEW_APPLYCLOSURE(&cond, EBUSY);
    leader->apply(task);
    cond.wait();
    //cluster.start(target.addr);
    usleep(1000 * 1000);
    LOG(INFO) << "here";
    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_EQ(saved_leader, leader);
    LOG(INFO) << "restart the target follower";
    cluster.start(target.addr);
    usleep(1000 * 1000);
    data.resize(5, 'a');
    task.data = &data;
    cond.reset(1);
    task.done = NEW_APPLYCLOSURE(&cond, 0);
    leader->apply(task);
    cond.wait();
    ASSERT_TRUE(cluster.ensure_same(5));
    cluster.stop_all();
}

class MockFSM1 : public MockFSM {
protected:
    MockFSM1() : MockFSM(butil::EndPoint()) {}
    virtual int on_snapshot_load(braft::SnapshotReader* reader) {
        (void)reader;
        return -1;
    }
};

TEST_F(NodeTest, shutdown_and_join_work_after_init_fails) {
    brpc::Server server;
    int ret = braft::add_service(&server, 5006);
    server.Start(5006, NULL);
    ASSERT_EQ(0, ret);

    braft::PeerId peer;
    peer.addr.ip = butil::my_ip();
    peer.addr.port = 5006;
    peer.idx = 0;
    std::vector<braft::PeerId> peers;
    peers.push_back(peer);

    {
        braft::NodeOptions options;
        options.election_timeout_ms = 300;
        options.initial_conf = braft::Configuration(peers);
        options.fsm = new MockFSM1();
        options.log_uri = "local://./data/log";
        options.raft_meta_uri = "local://./data/raft_meta";
        options.snapshot_uri = "local://./data/snapshot";
        braft::Node node("unittest", peer);
        ASSERT_EQ(0, node.init(options));
        sleep(1);
        bthread::CountdownEvent cond(10);
        for (int i = 0; i < 10; i++) {
            butil::IOBuf data;
            char data_buf[128];
            snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
            data.append(data_buf);
            braft::Task task;
            task.data = &data;
            task.done = NEW_APPLYCLOSURE(&cond, 0);
            node.apply(task);
        }
        cond.wait();
        LOG(INFO) << "begin to save snapshot";
        node.snapshot(NULL);
        LOG(INFO) << "begin to shutdown";
        node.shutdown(NULL);
        node.join();
    }
    
    {
        braft::NodeOptions options;
        options.election_timeout_ms = 300;
        options.initial_conf = braft::Configuration(peers);
        options.fsm = new MockFSM1();
        options.log_uri = "local://./data/log";
        options.raft_meta_uri = "local://./data/raft_meta";
        options.snapshot_uri = "local://./data/snapshot";
        braft::Node node("unittest", peer);
        LOG(INFO) << "node init again";
        ASSERT_NE(0, node.init(options));
        node.shutdown(NULL);
        node.join();
    }

    server.Stop(200);
    server.Join();
}

TEST_F(NodeTest, shutting_leader_triggers_timeout_now) {
    google::SetCommandLineOption("raft_sync", "false");
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;
        peers.push_back(peer);
    }
    // start cluster
    Cluster cluster("unittest", peers, 1000);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(INFO) << "shutdown leader" << leader->node_id();
    leader->shutdown(NULL);
    leader->join();
    LOG(INFO) << "join";
    usleep(100 * 1000);
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    google::SetCommandLineOption("raft_sync", "true");
}

TEST_F(NodeTest, removing_leader_triggers_timeout_now) {
    google::SetCommandLineOption("raft_sync", "false");
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;
        peers.push_back(peer);
    }
    // start cluster
    Cluster cluster("unittest", peers, 1000);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    braft::PeerId old_leader_id = leader->node_id().peer_id;
    LOG(WARNING) << "remove leader " << old_leader_id;
    bthread::CountdownEvent cond;
    leader->remove_peer(old_leader_id, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();
    usleep(100 * 1000);
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    ASSERT_NE(old_leader_id, leader->node_id().peer_id);
    google::SetCommandLineOption("raft_sync", "true");
}

TEST_F(NodeTest, transfer_should_work_after_install_snapshot) {
    std::vector<braft::PeerId> peers;
    for (size_t i = 0; i < 3; ++i) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;
        peers.push_back(peer);
    }
    // start cluster
    Cluster cluster("unittest", peers, 1000);
    for (size_t i = 0; i < peers.size() - 1; i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(1, nodes.size());
    braft::PeerId follower = nodes[0]->node_id().peer_id;
    leader->transfer_leadership_to(follower);
    usleep(2000 * 1000);
    leader = cluster.leader();
    ASSERT_EQ(follower, leader->node_id().peer_id);
    cond.reset(1);
    leader->snapshot(NEW_SNAPSHOTCLOSURE(&cond, 0));
    cond.wait();
    cond.reset(1);
    leader->snapshot(NEW_SNAPSHOTCLOSURE(&cond, 0));
    cond.wait();

    // Start the last peer which should be recover with snapshot
    braft::PeerId last_peer = peers.back(); 
    cluster.start(last_peer.addr);
    usleep(5000 * 1000);

    ASSERT_EQ(0, leader->transfer_leadership_to(last_peer));
    usleep(2000 * 1000);
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    ASSERT_EQ(last_peer, leader->node_id().peer_id);
}

TEST_F(NodeTest, append_entries_when_follower_is_in_error_state) {
    std::vector<braft::PeerId> peers;
    // five nodes
    for (int i = 0; i < 5; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;
        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    // set the first Follower to Error state
    std::vector<braft::Node*> nodes;
    cluster.followers(&nodes);
    ASSERT_EQ(nodes.size(), 4);
    butil::EndPoint error_follower = nodes[0]->node_id().peer_id.addr;
    braft::Node* error_follower_node = nodes[0];
    LOG(WARNING) << "set follower error " << nodes[0]->node_id();
    braft::NodeImpl *node_impl = nodes[0]->_impl;
    node_impl->AddRef();
    braft::Error e;
    e.set_type(braft::ERROR_TYPE_STATE_MACHINE);
    e.status().set_error(EINVAL, "Follower has something wrong");
    node_impl->on_error(e);
    node_impl->Release();

    // increase term  by stopping leader and electing a new leader again
    butil::EndPoint old_leader = leader->node_id().peer_id.addr;
    LOG(WARNING) << "stop leader " << leader->node_id();
    cluster.stop(old_leader);
    // elect new leader
    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "elect new leader " << leader->node_id();

    // apply something again 
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    sleep(2);
    // stop error follower
    LOG(WARNING) << "stop wrong follower " << error_follower_node->node_id();
    cluster.stop(error_follower);
    
    sleep(5);
    // restart error follower
    ASSERT_EQ(0, cluster.start(error_follower));
    LOG(WARNING) << "restart error follower " << error_follower;

    // restart old leader
    ASSERT_EQ(0, cluster.start(old_leader));
    LOG(WARNING) << "restart old leader " << old_leader;

    cluster.ensure_same();

    cluster.stop_all();
}

TEST_F(NodeTest, on_start_following_and_on_stop_following) {
    std::vector<braft::PeerId> peers;
    // five nodes
    for (int i = 0; i < 5; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;
        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader_first
    cluster.wait_leader();
    braft::Node* leader_first = cluster.leader();
    ASSERT_TRUE(leader_first != NULL);
    LOG(WARNING) << "leader_first is " << leader_first->node_id();

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader_first->apply(task);
    }
    cond.wait();
   
    // check _on_start_following_times and _on_stop_following_times
    std::vector<braft::Node*> followers_first;
    cluster.followers(&followers_first);
    ASSERT_EQ(followers_first.size(), 4);
    // leader_first's _on_start_following_times and _on_stop_following_times should both be 0.
    ASSERT_EQ(static_cast<MockFSM*>(leader_first->_impl->_options.fsm)->_on_start_following_times, 0);
    ASSERT_EQ(static_cast<MockFSM*>(leader_first->_impl->_options.fsm)->_on_stop_following_times, 0);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(static_cast<MockFSM*>(followers_first[i]->_impl->_options.fsm)->_on_start_following_times, 1);
        ASSERT_EQ(static_cast<MockFSM*>(followers_first[i]->_impl->_options.fsm)->_on_stop_following_times, 0);
    }

    // stop old leader and elect a new one
    butil::EndPoint leader_first_endpoint = leader_first->node_id().peer_id.addr;
    LOG(WARNING) << "stop leader_first " << leader_first->node_id();
    cluster.stop(leader_first_endpoint);
    // elect new leader
    cluster.wait_leader();
    braft::Node* leader_second = cluster.leader();
    ASSERT_TRUE(leader_second != NULL);
    LOG(WARNING) << "elect new leader " << leader_second->node_id();

    // apply something
    cond.reset(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader_second->apply(task);
    }
    cond.wait();
 
    // check _on_start_following_times and _on_stop_following_times again
    std::vector<braft::Node*> followers_second;
    cluster.followers(&followers_second);
    ASSERT_EQ(followers_second.size(), 3);
    // leader_second's _on_start_following_times and _on_stop_following_times should both be 1.
    // When it was still in follower state, it would do handle_election_timeout and
    // trigger on_stop_following when not receiving heartbeat for a long
    // time(election_timeout_ms).
    ASSERT_EQ(static_cast<MockFSM*>(leader_second->_impl->_options.fsm)->_on_start_following_times, 1);
    ASSERT_EQ(static_cast<MockFSM*>(leader_second->_impl->_options.fsm)->_on_stop_following_times, 1);
    for (int i = 0; i < 3; i++) {
        // Firstly these followers have a leader, but it stops and a candidate
        // sends request_vote_request to them, which triggers on_stop_following.
        // When the candidate becomes new leader, on_start_following is triggled
        // again so _on_start_following_times increase by 1.
        ASSERT_EQ(static_cast<MockFSM*>(followers_second[i]->_impl->_options.fsm)->_on_start_following_times, 2);
        ASSERT_EQ(static_cast<MockFSM*>(followers_second[i]->_impl->_options.fsm)->_on_stop_following_times, 1);
    }

    // transfer leadership to a follower
    braft::PeerId target = followers_second[0]->node_id().peer_id;
    ASSERT_EQ(0, leader_second->transfer_leadership_to(target));
    usleep(10 * 1000);
    cluster.wait_leader();
    braft::Node* leader_third = cluster.leader();
    ASSERT_EQ(target, leader_third->node_id().peer_id);
    
    // apply something
    cond.reset(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader_third->apply(task);
    }
    cond.wait();
    
    // check _on_start_following_times and _on_stop_following_times again 
    std::vector<braft::Node*> followers_third;
    cluster.followers(&followers_third);
    ASSERT_EQ(followers_second.size(), 3);
    // leader_third's _on_start_following_times and _on_stop_following_times should both be 2.
    // When it was still in follower state, it would do handle_timeout_now_request and
    // trigger on_stop_following when leader_second transferred leadership to it.
    ASSERT_EQ(static_cast<MockFSM*>(leader_third->_impl->_options.fsm)->_on_start_following_times, 2);
    ASSERT_EQ(static_cast<MockFSM*>(leader_third->_impl->_options.fsm)->_on_stop_following_times, 2);
    for (int i = 0; i < 3; i++) {
        // leader_second became follower when it transferred leadership to target, 
        // and when it receives leader_third's append_entries_request on_start_following is triggled.
        if (followers_third[i]->node_id().peer_id == leader_second->node_id().peer_id) {
            ASSERT_EQ(static_cast<MockFSM*>(followers_third[i]->_impl->_options.fsm)->_on_start_following_times, 2);
            ASSERT_EQ(static_cast<MockFSM*>(followers_third[i]->_impl->_options.fsm)->_on_stop_following_times, 1);
            continue;
        }
        // other followers just lose the leader_second and get leader_third, so _on_stop_following_times and 
        // _on_start_following_times both increase by 1. 
        ASSERT_EQ(static_cast<MockFSM*>(followers_third[i]->_impl->_options.fsm)->_on_start_following_times, 3);
        ASSERT_EQ(static_cast<MockFSM*>(followers_third[i]->_impl->_options.fsm)->_on_stop_following_times, 2);
    }

    cluster.ensure_same();
   
    cluster.stop_all();
}

TEST_F(NodeTest, read_committed_user_log) {
    std::vector<braft::PeerId> peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;
        peers.push_back(peer);
    }

    // start cluster
    Cluster cluster("unittest", peers);
    for (size_t i = 0; i < peers.size(); i++) {
        ASSERT_EQ(0, cluster.start(peers[i].addr));
    }

    // elect leader_first
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    ASSERT_TRUE(leader != NULL);
    LOG(WARNING) << "leader is " << leader->node_id();

    // apply something
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    sleep(2);
    
    // index == 1 is a CONFIGURATION log, so real_index will be 2 when returned.
    int64_t index = 1;
    braft::UserLog* user_log = new braft::UserLog();
    butil::Status status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(0, status.error_code());
    ASSERT_EQ(2, user_log->log_index());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 

    // index == 5 is a DATA log(a user log)
    index = 5;
    user_log->reset();
    status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(0, status.error_code());
    ASSERT_EQ(5, user_log->log_index());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 
    
    // index == 15 is greater than last_committed_index
    index = 15;
    user_log->reset();
    status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(braft::ENOMOREUSERLOG, status.error_code());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 
    
    // index == 0, invalid request index.
    index = 0;
    user_log->reset();
    status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(EINVAL, status.error_code());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 
   
    // trigger leader snapshot for the first time
    LOG(WARNING) << "trigger leader snapshot ";
    cond.reset(1);
    leader->snapshot(NEW_SNAPSHOTCLOSURE(&cond, 0));
    cond.wait();
    
    // remove and add a peer to add two CONFIGURATION logs
    std::vector<braft::Node*> followers;
    cluster.followers(&followers);
    braft::PeerId follower_test = followers[0]->node_id().peer_id;
    cond.reset(1);
    leader->remove_peer(follower_test, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();
    std::vector<braft::PeerId> new_peers;
    for (int i = 0; i < 3; i++) {
        braft::PeerId peer;
        peer.addr.ip = butil::my_ip();
        peer.addr.port = 5006 + i;
        peer.idx = 0;

        if (peer != follower_test) {
            new_peers.push_back(peer);
        }
    }
    cond.reset(1);
    leader->add_peer(follower_test, NEW_REMOVEPEERCLOSURE(&cond, 0));
    cond.wait();

    // apply something again
    cond.reset(10);
    for (int i = 10; i < 20; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    
    // trigger leader snapshot for the second time, after this the log of index 1~11 will be deleted.
    LOG(WARNING) << "trigger leader snapshot ";
    cond.reset(1);
    leader->snapshot(NEW_SNAPSHOTCLOSURE(&cond, 0));
    cond.wait();

    // index == 5 log has been deleted in log_storage.
    index = 5;
    user_log->reset();
    status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(braft::ELOGDELETED, status.error_code());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 
    
    // index == 12 and index == 13 are 2 CONFIGURATION logs, so real_index will be 14 when returned.
    index = 12;
    user_log->reset();
    status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(0, status.error_code());
    ASSERT_EQ(14, user_log->log_index());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 
    
    // now index == 15 is a user log
    index = 15;
    user_log->reset();
    status = leader->read_committed_user_log(index, user_log);
    ASSERT_EQ(0, status.error_code());
    ASSERT_EQ(15, user_log->log_index());
    LOG(INFO) << "read local committed user log from leader:" << leader->node_id() << ", index:"
        << index << ", real_index:" << user_log->log_index() << ", data:" << user_log->log_data() 
        << ", status:" << status; 
   
    delete(user_log);

    cluster.ensure_same();
    cluster.stop_all();
}

TEST_F(NodeTest, boostrap_with_snapshot) {
    butil::EndPoint addr;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:5006", &addr));
    MockFSM fsm(addr);
    for (char c = 'a'; c <= 'z'; ++c) {
        butil::IOBuf buf;
        buf.resize(100, c);
        fsm.logs.push_back(buf);
    }
    braft::BootstrapOptions boptions;
    boptions.last_log_index = fsm.logs.size();
    boptions.log_uri = "local://./data/log";
    boptions.raft_meta_uri = "local://./data/raft_meta";
    boptions.snapshot_uri = "local://./data/snapshot";
    boptions.group_conf.add_peer(braft::PeerId(addr));
    boptions.node_owns_fsm = false;
    boptions.fsm = &fsm;
    ASSERT_EQ(0, braft::bootstrap(boptions));
    brpc::Server server;
    ASSERT_EQ(0, braft::add_service(&server, addr));
    ASSERT_EQ(0, server.Start(addr, NULL));
    braft::Node node("test", braft::PeerId(addr));
    braft::NodeOptions options;
    options.log_uri = "local://./data/log";
    options.raft_meta_uri = "local://./data/raft_meta";
    options.snapshot_uri = "local://./data/snapshot";
    options.node_owns_fsm = false;
    options.fsm = &fsm;
    ASSERT_EQ(0, node.init(options));
    ASSERT_EQ(26u, fsm.logs.size());
    for (char c = 'a'; c <= 'z'; ++c) {
        std::string expected;
        expected.resize(100, c);
        ASSERT_TRUE(fsm.logs[c - 'a'].equals(expected));
    }
    while (!node.is_leader()) {
        usleep(1000);
    }
    node.shutdown(NULL);
    node.join();
}

TEST_F(NodeTest, boostrap_without_snapshot) {
    butil::EndPoint addr;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:5006", &addr));
    braft::BootstrapOptions boptions;
    boptions.last_log_index = 0;
    boptions.log_uri = "local://./data/log";
    boptions.raft_meta_uri = "local://./data/raft_meta";
    boptions.snapshot_uri = "local://./data/snapshot";
    boptions.group_conf.add_peer(braft::PeerId(addr));
    ASSERT_EQ(0, braft::bootstrap(boptions));
    brpc::Server server;
    ASSERT_EQ(0, braft::add_service(&server, addr));
    ASSERT_EQ(0, server.Start(addr, NULL));
    braft::Node node("test", braft::PeerId(addr));
    braft::NodeOptions options;
    options.log_uri = "local://./data/log";
    options.raft_meta_uri = "local://./data/raft_meta";
    options.snapshot_uri = "local://./data/snapshot";
    options.node_owns_fsm = false;
    MockFSM fsm(addr);
    options.fsm = &fsm;
    ASSERT_EQ(0, node.init(options));
    while (!node.is_leader()) {
        usleep(1000);
    }
    node.shutdown(NULL);
    node.join();
}

TEST_F(NodeTest, change_peers) {
    std::vector<braft::PeerId> peers;
    braft::PeerId peer0;
    peer0.addr.ip = butil::my_ip();
    peer0.addr.port = 5006;
    peer0.idx = 0;

    // start cluster
    peers.push_back(peer0);
    Cluster cluster("unittest", peers);
    ASSERT_EQ(0, cluster.start(peer0.addr));
    LOG(INFO) << "start single cluster " << peer0;
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();

    for (int i = 1; i < 10; ++i) {
        braft::PeerId peer = peer0;
        peer.addr.port += i;
        ASSERT_EQ(0, cluster.start(peer.addr, true));
    }
    for (int i = 0; i < 9; ++i) {
        cluster.wait_leader();
        braft::Node* leader = cluster.leader();
        braft::PeerId peer = peer0;
        peer.addr.port += i;
        ASSERT_EQ(leader->node_id().peer_id, peer);
        peer.addr.port += 1;
        braft::Configuration conf;
        conf.add_peer(peer);
        braft::SynchronizedClosure done;
        leader->change_peers(conf, &done);
        done.wait();
        ASSERT_TRUE(done.status().ok()) << done.status();
    }
    ASSERT_TRUE(cluster.ensure_same());
}

TEST_F(NodeTest, change_peers_add_multiple_node) {
    std::vector<braft::PeerId> peers;
    braft::PeerId peer0;
    peer0.addr.ip = butil::my_ip();
    peer0.addr.port = 5006;
    peer0.idx = 0;

    // start cluster
    peers.push_back(peer0);
    Cluster cluster("unittest", peers);
    ASSERT_EQ(0, cluster.start(peer0.addr));
    LOG(INFO) << "start single cluster " << peer0;
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    braft::Configuration conf;
    for (int i = 0; i < 3; ++i) {
        braft::PeerId peer = peer0;
        peer.addr.port += i;
        conf.add_peer(peer);
    }
    cluster.wait_leader();
    braft::SynchronizedClosure done;
    leader->change_peers(conf, &done);
    done.wait();
    ASSERT_EQ(braft::ECATCHUP, done.status().error_code()) << done.status();
    braft::PeerId peer = peer0;
    peer.addr.port++;
    cluster.start(peer.addr, false);
    done.reset();
    leader->change_peers(conf, &done);
    done.wait();
    ASSERT_EQ(braft::ECATCHUP, done.status().error_code()) << done.status();
    peer.addr.port++;
    cluster.start(peer.addr, false);
    done.reset();
    leader->change_peers(conf, &done);
    done.wait();
    ASSERT_TRUE(done.status().ok()) << done.status();
    ASSERT_TRUE(cluster.ensure_same());
}

TEST_F(NodeTest, change_peers_steps_down_in_joint_consensus) {
    std::vector<braft::PeerId> peers;
    braft::PeerId peer0("127.0.0.1:5006");
    braft::PeerId peer1("127.0.0.1:5007");
    braft::PeerId peer2("127.0.0.1:5008");
    braft::PeerId peer3("127.0.0.1:5009");

    // start cluster
    peers.push_back(peer0);
    Cluster cluster("unittest", peers);
    ASSERT_EQ(0, cluster.start(peer0.addr));
    LOG(INFO) << "start single cluster " << peer0;
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    bthread::CountdownEvent cond(10);
    for (int i = 0; i < 10; i++) {
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        task.data = &data;
        task.done = NEW_APPLYCLOSURE(&cond, 0);
        leader->apply(task);
    }
    cond.wait();
    cluster.start(peer1.addr, false);
    cluster.start(peer2.addr, false);
    cluster.start(peer3.addr, false);
    braft::Configuration conf;
    conf.add_peer(peer0);
    conf.add_peer(peer1);
    conf.add_peer(peer2);
    conf.add_peer(peer3);
    braft::SynchronizedClosure done;
    leader->change_peers(conf, &done);
    done.wait();
    ASSERT_TRUE(done.status().ok());
    ASSERT_EQ(0, cluster.stop(peer3.addr));
    conf.remove_peer(peer0);
    conf.remove_peer(peer1);

    // Change peers to [peer2, peer3], which must fail since peer3 is stopped
    done.reset();
    leader->change_peers(conf, &done);
    done.wait();
    ASSERT_EQ(EPERM, done.status().error_code());
    ASSERT_FALSE(leader->_impl->_conf.stable());
    LOG(INFO) << done.status();
    leader = cluster.leader();
    ASSERT_TRUE(leader == NULL);
    cluster.start(peer3.addr, false);
    usleep(1000 * 1000);  // Temporarily solution
    cluster.wait_leader();
    leader = cluster.leader();
    ASSERT_TRUE(leader->list_peers(&peers).ok());
    ASSERT_TRUE(conf.equals(peers));
    ASSERT_FALSE(leader->_impl->_conf_ctx.is_busy());
}

struct ChangeArg {
    Cluster* c;
    std::vector<braft::PeerId> peers;
    volatile bool stop;
    bool dont_remove_first_peer;
};

static void* change_routine(void* arg) {
    ChangeArg* ca = (ChangeArg*)arg;
    while (!ca->stop) {
        ca->c->wait_leader();
        braft::Node* leader = ca->c->leader();
        if (!leader) {
            continue;
        }
        // Randomly select peers
        braft::Configuration conf;
        if (ca->dont_remove_first_peer) {
            conf.add_peer(ca->peers[0]);
        }
        for (size_t i = 0; i < ca->peers.size(); ++i) {
            bool select = butil::fast_rand_less_than(64) < 32;
            if (select) {
                conf.add_peer(ca->peers[i]);
            }
        }
        if (conf.empty()) {
            LOG(WARNING) << "No peer has been selected";
            // Bad luck here
            continue;
        }
        braft::SynchronizedClosure done;
        leader->change_peers(conf, &done);
        done.wait();
        CHECK(done.status().ok());
    }
    return NULL;
}

TEST_F(NodeTest, change_peers_chaos_with_snapshot) {
    g_dont_print_apply_log = true;
    google::SetCommandLineOption("raft_sync", "false");
    ASSERT_FALSE(google::SetCommandLineOption("crash_on_fatal_log", "true").empty());
    google::SetCommandLineOption("minloglevel", "3");
    std::vector<braft::PeerId> peers;
    // start cluster
    peers.push_back(braft::PeerId("127.0.0.1:5006"));
    Cluster cluster("unittest", peers, 1000);
    cluster.start(peers.front().addr, false, 1);
    for (int i = 1; i < 10; ++i) {
        peers.push_back("127.0.0.1:" + std::to_string(5006 + i));
        cluster.start(peers.back().addr, true, 1);
    }
    ChangeArg arg;
    arg.c = &cluster;
    arg.peers = peers;
    arg.stop = false;
    arg.dont_remove_first_peer = false;
    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, change_routine, &arg));
    for (int i = 0; i < 1000;) {
        cluster.wait_leader();
        braft::Node* leader = cluster.leader();
        if (!leader) {
            continue;
        }
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        braft::SynchronizedClosure done;
        task.data = &data;
        task.done = &done;
        leader->apply(task);
        done.wait();
        if (done.status().ok()) {
            ++i;
        } else {
            EXPECT_EQ(EPERM, done.status().error_code());
        }
    }
    arg.stop = true;
    pthread_join(tid, NULL);
    google::SetCommandLineOption("raft_sync", "true");
    google::SetCommandLineOption("minloglevel", "3");
}

TEST_F(NodeTest, change_peers_chaos_without_snapshot) {
    g_dont_print_apply_log = true;
    google::SetCommandLineOption("minloglevel", "3");
    google::SetCommandLineOption("raft_sync", "false");
    ASSERT_FALSE(google::SetCommandLineOption("crash_on_fatal_log", "true").empty());
    std::vector<braft::PeerId> peers;
    // start cluster
    peers.push_back(braft::PeerId("127.0.0.1:5006"));
    Cluster cluster("unittest", peers, 1000);
    cluster.start(peers.front().addr, false, 100000);
    for (int i = 1; i < 10; ++i) {
        peers.push_back("127.0.0.1:" + std::to_string(5006 + i));
        cluster.start(peers.back().addr, true, 10000);
    }
    ChangeArg arg;
    arg.c = &cluster;
    arg.peers = peers;
    arg.stop = false;
    arg.dont_remove_first_peer = true;
    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, change_routine, &arg));
    for (int i = 0; i < 100000;) {
        cluster.wait_leader();
        braft::Node* leader = cluster.leader();
        if (!leader) {
            continue;
        }
        butil::IOBuf data;
        char data_buf[128];
        snprintf(data_buf, sizeof(data_buf), "hello: %d", i + 1);
        data.append(data_buf);
        braft::Task task;
        braft::SynchronizedClosure done;
        task.data = &data;
        task.done = &done;
        leader->apply(task);
        done.wait();
        if (done.status().ok()) {
            ++i;
        } else {
            EXPECT_EQ(EPERM, done.status().error_code());
        }
    }
    arg.stop = true;
    pthread_join(tid, NULL);
    cluster.wait_leader();
    braft::Node* leader = cluster.leader();
    braft::SynchronizedClosure done;
    leader->change_peers(braft::Configuration(peers), &done);
    done.wait();
    ASSERT_TRUE(done.status().ok()) << done.status();
    cluster.ensure_same();
    std::cout << "Stopping cluster" << std::endl;
    cluster.stop_all();
    google::SetCommandLineOption("raft_sync", "true");
    google::SetCommandLineOption("minloglevel", "0");
}