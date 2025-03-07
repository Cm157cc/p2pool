/*
 * This file is part of the Monero P2Pool <https://github.com/SChernykh/p2pool>
 * Copyright (c) 2021 SChernykh <https://github.com/SChernykh>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "p2pool.h"
#include "zmq_reader.h"
#include "mempool.h"
#include "json_rpc_request.h"
#include "rapidjson/document.h"
#include "json_parsers.h"
#include "pow_hash.h"
#include "block_template.h"
#include "side_chain.h"
#include "stratum_server.h"
#include "p2p_server.h"
#include "params.h"
#include "console_commands.h"
#include "crypto.h"
#include "p2pool_api.h"
#include <thread>
#include <fstream>

constexpr char log_category_prefix[] = "P2Pool ";
constexpr int BLOCK_HEADERS_REQUIRED = 720;

constexpr uint64_t SEEDHASH_EPOCH_BLOCKS = 2048;
constexpr uint64_t SEEDHASH_EPOCH_LAG = 64;

constexpr char FOUND_BLOCKS_FILE[] = "p2pool.blocks";

namespace p2pool {

p2pool::p2pool(int argc, char* argv[])
	: m_stopped(false)
	, m_params(new Params(argc, argv))
	, m_updateSeed(true)
	, m_submitBlockData{}
	, m_zmqLastActive(0)
	, m_startTime(time(nullptr))
{
	LOGINFO(1, log::LightCyan() << VERSION);

	if (!m_params->m_wallet.valid()) {
		LOGERR(1, "Invalid wallet address. Try \"p2pool --help\".");
		panic();
	}

	bool is_v6;
	if (!resolve_host(m_params->m_host, is_v6)) {
		LOGERR(1, "resolve_host failed for " << m_params->m_host);
		panic();
	}

	hash pub, sec, eph_public_key;
	generate_keys(pub, sec);

	if (!m_params->m_wallet.get_eph_public_key(sec, 0, eph_public_key)) {
		LOGERR(1, "Invalid wallet address: get_eph_public_key failed");
		panic();
	}

	const NetworkType type = m_params->m_wallet.type();

	if (type == NetworkType::Testnet) {
		LOGWARN(1, "Mining to a testnet wallet address");
	}
	else if (type == NetworkType::Stagenet) {
		LOGWARN(1, "Mining to a stagenet wallet address");
	}

	int err = uv_async_init(uv_default_loop_checked(), &m_submitBlockAsync, on_submit_block);
	if (err) {
		LOGERR(1, "uv_async_init failed, error " << uv_err_name(err));
		panic();
	}
	m_submitBlockAsync.data = this;

	err = uv_async_init(uv_default_loop_checked(), &m_blockTemplateAsync, on_update_block_template);
	if (err) {
		LOGERR(1, "uv_async_init failed, error " << uv_err_name(err));
		panic();
	}
	m_blockTemplateAsync.data = this;

	err = uv_async_init(uv_default_loop_checked(), &m_stopAsync, on_stop);
	if (err) {
		LOGERR(1, "uv_async_init failed, error " << uv_err_name(err));
		panic();
	}
	m_stopAsync.data = this;

	uv_rwlock_init_checked(&m_mainchainLock);
	uv_mutex_init_checked(&m_foundBlocksLock);
	uv_mutex_init_checked(&m_submitBlockDataLock);

	m_api = m_params->m_apiPath.empty() ? nullptr : new p2pool_api(m_params->m_apiPath, m_params->m_localStats);

	m_sideChain = new SideChain(this, type);
	m_hasher = new RandomX_Hasher(this);
	m_blockTemplate = new BlockTemplate(this);
	m_mempool = new Mempool();
	m_consoleCommands = new ConsoleCommands(this);
}

p2pool::~p2pool()
{
	uv_rwlock_destroy(&m_mainchainLock);
	uv_mutex_destroy(&m_foundBlocksLock);
	uv_mutex_destroy(&m_submitBlockDataLock);

	delete m_api;
	delete m_sideChain;
	delete m_hasher;
	delete m_blockTemplate;
	delete m_mempool;
	delete m_params;
	delete m_consoleCommands;
}

bool p2pool::calculate_hash(const void* data, size_t size, const hash& seed, hash& result)
{
	return m_hasher->calculate(data, size, seed, result);
}

uint64_t p2pool::get_seed_height(uint64_t height)
{
	if (LIKELY(height > SEEDHASH_EPOCH_LAG)) {
		return (height - SEEDHASH_EPOCH_LAG - 1) & ~(SEEDHASH_EPOCH_BLOCKS - 1);
	}
	return 0;
}

bool p2pool::get_seed(uint64_t height, hash& seed) const
{
	ReadLock lock(m_mainchainLock);

	auto it = m_mainchainByHeight.find(get_seed_height(height));
	if (it == m_mainchainByHeight.end()) {
		return false;
	}

	seed = it->second.id;
	return true;
}

void p2pool::handle_tx(TxMempoolData& tx)
{
	if (!tx.weight || !tx.fee) {
		LOGWARN(1, "invalid transaction: tx id = " << tx.id << ", size = " << tx.blob_size << ", weight = " << tx.weight << ", fee = " << static_cast<double>(tx.fee) / 1e6 << " um");
		return;
	}

	m_mempool->add(tx);

	LOGINFO(5,
		"new tx id = " << log::LightBlue() << tx.id << log::NoColor() <<
		", size = " << log::Gray() << tx.blob_size << log::NoColor() <<
		", weight = " << log::Gray() << tx.weight << log::NoColor() <<
		", fee = " << log::Gray() << static_cast<double>(tx.fee) / 1e6 << " um");

#if TEST_MEMPOOL_PICKING_ALGORITHM
	m_blockTemplate->update(m_minerData, *m_mempool, &m_params->m_wallet);
#endif

	m_zmqLastActive = time(nullptr);
}

void p2pool::handle_miner_data(MinerData& data)
{
#if TEST_MEMPOOL_PICKING_ALGORITHM
	if (m_mempool->m_transactions.size() < data.tx_backlog.size()) {
		m_mempool->swap(data.tx_backlog);
	}
#else
	m_mempool->swap(data.tx_backlog);
#endif

	{
		WriteLock lock(m_mainchainLock);

		m_mainchainByHeight[data.height].difficulty = data.difficulty;

		ChainMain& c = m_mainchainByHeight[data.height - 1];
		c.height = data.height - 1;
		c.id = data.prev_id;

		// timestamp and reward is unknown here
		c.timestamp = 0;
		c.reward = 0;

		m_mainchainByHash[c.id] = c;

		cleanup_mainchain_data(data.height);
	}

	data.tx_backlog.clear();
	data.time_received = std::chrono::system_clock::now();
	m_minerData = data;
	m_updateSeed = true;
	update_median_timestamp();

	LOGINFO(2,
		"new miner data\n---------------------------------------------------------------------------------------------------------------" <<
		"\nmajor_version           = " << data.major_version <<
		"\nheight                  = " << data.height <<
		"\nprev_id                 = " << log::LightBlue() << data.prev_id << log::NoColor() <<
		"\nseed_hash               = " << log::LightBlue() << data.seed_hash << log::NoColor() <<
		"\ndifficulty              = " << data.difficulty <<
		"\nmedian_weight           = " << data.median_weight <<
		"\nalready_generated_coins = " << data.already_generated_coins <<
		"\ntransactions            = " << m_mempool->m_transactions.size() <<
		"\n---------------------------------------------------------------------------------------------------------------"
	);

	if (!is_main_thread()) {
		update_block_template_async();
	}
	else {
		update_block_template();
	}

	m_zmqLastActive = time(nullptr);

	if (m_serversStarted.load()) {
		std::vector<uint64_t> missing_heights;
		{
			WriteLock lock(m_mainchainLock);

			for (uint64_t h = data.height; h && (h + BLOCK_HEADERS_REQUIRED > data.height); --h) {
				if (m_mainchainByHeight.find(h) == m_mainchainByHeight.end()) {
					LOGWARN(3, "Mainchain data for height " << h << " is missing, requesting it from monerod again");
					missing_heights.push_back(h);
				}
			}
		}

		for (uint64_t h : missing_heights) {
			char buf[log::Stream::BUF_SIZE + 1];
			log::Stream s(buf);
			s << "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"get_block_header_by_height\",\"params\":{\"height\":" << h << "}}\0";

			JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, buf,
				[this, h](const char* data, size_t size)
				{
					ChainMain block;
					if (!parse_block_header(data, size, block)) {
						LOGERR(1, "couldn't download block header for height " << h);
					}
				},
				[h](const char* data, size_t size)
				{
					if (size > 0) {
						LOGERR(1, "couldn't download block header for height " << h << ", error " << log::const_buf(data, size));
					}
				});
		}
	}
}

const char* BLOCK_FOUND = "\n\
-----------------------------------------------------------------------------------------------\n\
| ######   #        #######   #####   #    #      #######  #######  #     #  #     #  ######  |\n\
| #     #  #        #     #  #     #  #   #       #        #     #  #     #  ##    #  #     # |\n\
| #     #  #        #     #  #        #  #        #        #     #  #     #  # #   #  #     # |\n\
| ######   #        #     #  #        ###         #####    #     #  #     #  #  #  #  #     # |\n\
| #     #  #        #     #  #        #  #        #        #     #  #     #  #   # #  #     # |\n\
| #     #  #        #     #  #     #  #   #       #        #     #  #     #  #    ##  #     # |\n\
| ######   #######  #######   #####   #    #      #        #######   #####   #     #  ######  |\n\
-----------------------------------------------------------------------------------------------";

void p2pool::handle_chain_main(ChainMain& data, const char* extra)
{
	{
		WriteLock lock(m_mainchainLock);

		ChainMain& c = m_mainchainByHeight[data.height];
		c.height = data.height;
		c.timestamp = data.timestamp;
		c.reward = data.reward;

		// data.id not filled in here, but c.id should be available. Copy it to data.id for logging
		data.id = c.id;

		m_mainchainByHash[c.id] = c;
	}
	update_median_timestamp();

	hash sidechain_id;
	if (extra) {
		const size_t n = strlen(extra);
		if (n >= HASH_SIZE * 2) {
			const char* s = extra + n - HASH_SIZE * 2;
			for (size_t i = 0; i < HASH_SIZE; ++i) {
				uint8_t d[2];
				if (!from_hex(s[i * 2], d[0]) || !from_hex(s[i * 2 + 1], d[1])) {
					sidechain_id = {};
					break;
				}
				sidechain_id.h[i] = (d[0] << 4) | d[1];
			}
		}
	}

	LOGINFO(2, "new main chain block: height = " << log::Gray() << data.height << log::NoColor() <<
		", id = " << log::LightBlue() << data.id << log::NoColor() <<
		", timestamp = " << log::Gray() << data.timestamp << log::NoColor() << 
		", reward = " << log::Gray() << log::XMRAmount(data.reward));

	if (!sidechain_id.empty()) {
		if (side_chain().has_block(sidechain_id)) {
			LOGINFO(0, log::LightGreen() << "BLOCK FOUND: main chain block at height " << data.height << " was mined by this p2pool" << BLOCK_FOUND);
			api_update_block_found(&data);
		}
		else {
			side_chain().watch_mainchain_block(data, sidechain_id);
		}
	}

	api_update_network_stats();

	m_zmqLastActive = time(nullptr);
}

void p2pool::submit_block_async(uint32_t template_id, uint32_t nonce, uint32_t extra_nonce)
{
	{
		MutexLock lock(m_submitBlockDataLock);

		m_submitBlockData.template_id = template_id;
		m_submitBlockData.nonce = nonce;
		m_submitBlockData.extra_nonce = extra_nonce;
		m_submitBlockData.blob.clear();
	}

	const int err = uv_async_send(&m_submitBlockAsync);
	if (err) {
		LOGERR(1, "uv_async_send failed, error " << uv_err_name(err));
	}
}

void p2pool::submit_block_async(const std::vector<uint8_t>& blob)
{
	{
		MutexLock lock(m_submitBlockDataLock);

		m_submitBlockData.template_id = 0;
		m_submitBlockData.nonce = 0;
		m_submitBlockData.extra_nonce = 0;
		m_submitBlockData.blob = blob;
	}

	const int err = uv_async_send(&m_submitBlockAsync);
	if (err) {
		LOGERR(1, "uv_async_send failed, error " << uv_err_name(err));
	}
}

void p2pool::on_stop(uv_async_t* async)
{
	p2pool* pool = reinterpret_cast<p2pool*>(async->data);

	if (pool->m_api) {
		pool->m_api->on_stop();
	}

	uv_close(reinterpret_cast<uv_handle_t*>(&pool->m_submitBlockAsync), nullptr);
	uv_close(reinterpret_cast<uv_handle_t*>(&pool->m_blockTemplateAsync), nullptr);
	uv_close(reinterpret_cast<uv_handle_t*>(&pool->m_stopAsync), nullptr);
	uv_stop(uv_default_loop());
}

void p2pool::submit_block() const
{
	SubmitBlockData submit_data;
	{
		MutexLock lock(m_submitBlockDataLock);
		submit_data = m_submitBlockData;
	}

	const uint64_t height = m_blockTemplate->height();
	const difficulty_type diff = m_blockTemplate->difficulty();

	size_t nonce_offset = 0;
	size_t extra_nonce_offset = 0;
	bool is_external = false;

	if (submit_data.blob.empty()) {
		LOGINFO(0, "submit_block: height = " << height << ", template id = " << submit_data.template_id << ", nonce = " << submit_data.nonce << ", extra_nonce = " << submit_data.extra_nonce);

		submit_data.blob = m_blockTemplate->get_block_template_blob(submit_data.template_id, nonce_offset, extra_nonce_offset);
		if (submit_data.blob.empty()) {
			LOGERR(0, "submit_block: couldn't find block template with id " << submit_data.template_id);
			return;
		}
	}
	else {
		LOGINFO(0, "submit_block: height = " << height << ", external blob (" << submit_data.blob.size() << " bytes)");
		is_external = true;
	}

	std::string request;
	request.reserve(submit_data.blob.size() * 2 + 128);

	request = "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"submit_block\",\"params\":[\"";

	const uint32_t template_id = submit_data.template_id;
	const uint32_t nonce = submit_data.nonce;
	const uint32_t extra_nonce = submit_data.extra_nonce;

	for (size_t i = 0; i < submit_data.blob.size(); ++i) {
		char buf[16];

		if (nonce_offset && nonce_offset <= i && i < nonce_offset + sizeof(submit_data.nonce)) {
			snprintf(buf, sizeof(buf), "%02x", submit_data.nonce & 255);
			submit_data.nonce >>= 8;
		}
		else if (extra_nonce_offset && extra_nonce_offset <= i && i < extra_nonce_offset + sizeof(submit_data.extra_nonce)) {
			snprintf(buf, sizeof(buf), "%02x", submit_data.extra_nonce & 255);
			submit_data.extra_nonce >>= 8;
		}
		else {
			snprintf(buf, sizeof(buf), "%02x", submit_data.blob[i]);
		}

		request.append(buf);
	}
	request.append("\"]}");

	JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, request.c_str(),
		[height, diff, template_id, nonce, extra_nonce, is_external](const char* data, size_t size)
		{
			rapidjson::Document doc;
			if (doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(data, size).HasParseError() || !doc.IsObject()) {
				LOGERR(0, "submit_block: invalid JSON response from daemon");
				return;
			}

			if (doc.HasMember("error")) {
				auto& err = doc["error"];

				if (!err.IsObject()) {
					LOGERR(0, "submit_block: invalid JSON reponse from daemon: 'error' is not an object");
					return;
				}

				const char* error_msg = nullptr;

				auto it = doc.FindMember("message");
				if (it != doc.MemberEnd() && it->value.IsString()) {
					error_msg = it->value.GetString();
				}

				if (is_external) {
					LOGWARN(3, "submit_block (external blob): daemon returned error: " << (error_msg ? error_msg : "unknown error"));
				}
				else {
					LOGERR(0, "submit_block: daemon returned error: '" << (error_msg ? error_msg : "unknown error") << "', template id = " << template_id << ", nonce = " << nonce << ", extra_nonce = " << extra_nonce);
				}
				return;
			}

			auto it = doc.FindMember("result");
			if (it != doc.MemberEnd() && it->value.IsObject()) {
				auto& result = it->value;
				auto it2 = result.FindMember("status");
				if (it2 != result.MemberEnd() && it2->value.IsString() && (strcmp(it2->value.GetString(), "OK") == 0)) {
					LOGINFO(0, log::LightGreen() << "submit_block: BLOCK ACCEPTED at height " << height << " and difficulty = " << diff);
					return;
				}
			}

			LOGWARN(0, "submit_block: daemon sent unrecognizable reply: " << log::const_buf(data, size));
		},
		[is_external](const char* data, size_t size)
		{
			if (size > 0) {
				if (is_external) {
					LOGWARN(3, "submit_block (external blob): RPC request failed, error " << log::const_buf(data, size));
				}
				else {
					LOGERR(0, "submit_block (external blob): RPC request failed, error " << log::const_buf(data, size));
				}
			}
		});
}

void p2pool::submit_sidechain_block(uint32_t template_id, uint32_t nonce, uint32_t extra_nonce)
{
	LOGINFO(3, "submit_sidechain_block: template id = " << template_id << ", nonce = " << nonce << ", extra_nonce = " << extra_nonce);
	m_blockTemplate->submit_sidechain_block(template_id, nonce, extra_nonce);
}

void p2pool::update_block_template_async()
{
	const int err = uv_async_send(&m_blockTemplateAsync);
	if (err) {
		LOGERR(1, "uv_async_send failed, error " << uv_err_name(err));
	}
}

void p2pool::update_block_template()
{
	if (m_updateSeed) {
		m_hasher->set_seed_async(m_minerData.seed_hash);
		m_updateSeed = false;
	}
	m_blockTemplate->update(m_minerData, *m_mempool, &m_params->m_wallet);
	stratum_on_block();
	api_update_pool_stats();
}

void p2pool::download_block_headers(uint64_t current_height)
{
	const uint64_t seed_height = get_seed_height(current_height);
	const uint64_t prev_seed_height = (seed_height > SEEDHASH_EPOCH_BLOCKS) ? (seed_height - SEEDHASH_EPOCH_BLOCKS) : 0;

	char buf[log::Stream::BUF_SIZE + 1];
	log::Stream s(buf);

	// First download 2 RandomX seeds
	const uint64_t seed_heights[2] = { prev_seed_height, seed_height };
	for (uint64_t height : seed_heights) {
		s.m_pos = 0;
		s << "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"get_block_header_by_height\",\"params\":{\"height\":" << height << "}}\0";

		JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, buf,
			[this, prev_seed_height, height](const char* data, size_t size)
			{
				ChainMain block;
				if (parse_block_header(data, size, block)) {
					if (height == prev_seed_height) {
						// Do it synchronously to make sure stratum and p2p don't start before it's finished
						m_hasher->set_old_seed(block.id);
					}
				}
				else {
					LOGERR(1, "fatal error: couldn't download block header for height " << height);
					panic();
				}
			},
			[height](const char* data, size_t size)
			{
				if (size > 0) {
					LOGERR(1, "fatal error: couldn't download block header for height " << height << ", error " << log::const_buf(data, size));
					panic();
				}
			});
	}

	s.m_pos = 0;
	s << "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"get_block_headers_range\",\"params\":{\"start_height\":" << current_height - BLOCK_HEADERS_REQUIRED << ",\"end_height\":" << current_height - 1 << "}}\0";

	JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, buf,
		[this, current_height](const char* data, size_t size)
		{
			if (parse_block_headers_range(data, size) == BLOCK_HEADERS_REQUIRED) {
				update_median_timestamp();
				if (m_serversStarted.exchange(1) == 0) {
					m_ZMQReader = new ZMQReader(m_params->m_host.c_str(), m_params->m_zmqPort, this);
					m_stratumServer = new StratumServer(this);
					m_p2pServer = new P2PServer(this);
					api_update_network_stats();
				}
			}
			else {
				LOGERR(1, "fatal error: couldn't download block headers for heights " << current_height - BLOCK_HEADERS_REQUIRED << " - " << current_height - 1);
				panic();
			}
		},
		[current_height](const char* data, size_t size)
		{
			if (size > 0) {
				LOGERR(1, "fatal error: couldn't download block headers for heights " << current_height - BLOCK_HEADERS_REQUIRED << " - " << current_height - 1 << ", error " << log::const_buf(data, size));
				panic();
			}
		});
}

bool p2pool::chainmain_get_by_hash(const hash& id, ChainMain& data) const
{
	ReadLock lock(m_mainchainLock);

	auto it = m_mainchainByHash.find(id);
	if (it == m_mainchainByHash.end()) {
		return false;
	}

	data = it->second;
	return true;
}

bool p2pool::get_timestamps(uint64_t (&timestamps)[TIMESTAMP_WINDOW]) const
{
	ReadLock lock(m_mainchainLock);

	if (m_mainchainByHeight.size() <= TIMESTAMP_WINDOW) {
		return false;
	}

	auto it = m_mainchainByHeight.end();

	for (int i = 0; (i < TIMESTAMP_WINDOW) && (it != m_mainchainByHeight.begin()); ++i) {
		--it;
		timestamps[i] = it->second.timestamp;
	}

	return true;
}

void p2pool::update_median_timestamp()
{
	uint64_t timestamps[TIMESTAMP_WINDOW];
	if (!get_timestamps(timestamps))
	{
		m_minerData.median_timestamp = 0;
		return;
	}

	std::sort(timestamps, timestamps + TIMESTAMP_WINDOW);

	// Shift it +1 block compared to Monero's code because we don't have the latest block yet when we receive new miner data
	m_minerData.median_timestamp = (timestamps[TIMESTAMP_WINDOW / 2] + timestamps[TIMESTAMP_WINDOW / 2 + 1]) / 2;
	LOGINFO(4, "median timestamp updated to " << log::Gray() << m_minerData.median_timestamp);
}

void p2pool::stratum_on_block()
{
#if 0
	uint8_t hashing_blob[128];
	uint64_t height;
	difficulty_type difficulty;
	difficulty_type sidechain_difficulty;
	hash seed_hash;
	size_t nonce_offset;
	uint32_t template_id;

	m_blockTemplate->get_hashing_blob(0, hashing_blob, height, difficulty, sidechain_difficulty, seed_hash, nonce_offset, template_id);
	submit_block(template_id, 0, 0);
#else
	if (m_stratumServer) {
		m_stratumServer->on_block(*m_blockTemplate);
	}
#endif
}

void p2pool::get_info()
{
	JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"get_info\"}",
		[this](const char* data, size_t size)
		{
			parse_get_info_rpc(data, size);
		},
		[this](const char* data, size_t size)
		{
			if (size > 0) {
				LOGWARN(1, "get_info RPC request failed: error " << log::const_buf(data, size) << ", trying again in 1 second");
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				get_info();
			}
		});
}

void p2pool::load_found_blocks()
{
	if (!m_api) {
		return;
	}

	std::ifstream f(FOUND_BLOCKS_FILE);
	if (!f.is_open()) {
		return;
	}

	while (!f.eof()) {
		time_t timestamp;
		f >> timestamp;
		if (f.eof()) break;

		uint64_t height;
		f >> height;
		if (f.eof()) break;

		hash id;
		f >> id;
		if (f.eof()) break;

		difficulty_type block_difficulty;
		f >> block_difficulty;
		if (f.eof()) break;

		difficulty_type cumulative_difficulty;
		f >> cumulative_difficulty;

		m_foundBlocks.emplace_back(timestamp, height, id, block_difficulty, cumulative_difficulty);
	}

	api_update_block_found(nullptr);
}

void p2pool::parse_get_info_rpc(const char* data, size_t size)
{
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(data, size);

	if (!doc.IsObject() || !doc.HasMember("result")) {
		LOGWARN(1, "get_info RPC response is invalid (\"result\" not found), trying again in 1 second");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		get_info();
		return;
	}

	const auto& result = doc["result"];

	struct {
		bool busy_syncing, synchronized, mainnet, testnet, stagenet;
	} info;

	if (!PARSE(result, info, busy_syncing) ||
		!PARSE(result, info, synchronized) ||
		!PARSE(result, info, mainnet) ||
		!PARSE(result, info, testnet) ||
		!PARSE(result, info, stagenet)) {
		LOGWARN(1, "get_info RPC response is invalid, trying again in 1 second");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		get_info();
		return;
	}

	if (info.busy_syncing || !info.synchronized) {
		LOGINFO(1, "monerod is " << (info.busy_syncing ? "busy syncing" : "not synchronized") << ", trying again in 1 second");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		get_info();
		return;
	}

	NetworkType monero_network = NetworkType::Invalid;

	if (info.mainnet)  monero_network = NetworkType::Mainnet;
	if (info.testnet)  monero_network = NetworkType::Testnet;
	if (info.stagenet) monero_network = NetworkType::Stagenet;

	const NetworkType sidechain_network = m_sideChain->network_type();

	if (monero_network != sidechain_network) {
		LOGERR(1, "monerod is on " << monero_network << ", but you're mining to a " << sidechain_network << " sidechain");
		panic();
	}

	get_version();
}

void p2pool::get_version()
{
	JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"get_version\"}",
		[this](const char* data, size_t size)
		{
			parse_get_version_rpc(data, size);
		},
		[this](const char* data, size_t size)
		{
			if (size > 0) {
				LOGWARN(1, "get_version RPC request failed: error " << log::const_buf(data, size) << ", trying again in 1 second");
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				get_version();
			}
		});
}

void p2pool::parse_get_version_rpc(const char* data, size_t size)
{
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(data, size);

	if (!doc.IsObject() || !doc.HasMember("result")) {
		LOGWARN(1, "get_version RPC response is invalid (\"result\" not found), trying again in 1 second");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		get_version();
		return;
	}

	const auto& result = doc["result"];

	std::string status;
	uint64_t version;

	if (!parseValue(result, "status", status) || !parseValue(result, "version", version)) {
		LOGWARN(1, "get_version RPC response is invalid, trying again in 1 second");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		get_version();
		return;
	}

	if (status != "OK") {
		LOGWARN(1, "get_version RPC failed, trying again in 1 second");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		get_version();
		return;
	}

	if (version < 0x30008) {
		const uint64_t version_hi = version >> 16;
		const uint64_t version_lo = version & 65535;
		LOGERR(1, "monerod RPC v" << version_hi << '.' << version_lo << " is incompatible, update to RPC >= v3.8");
		panic();
	}

	get_miner_data();
}

void p2pool::get_miner_data()
{
	JSONRPCRequest::call(m_params->m_host.c_str(), m_params->m_rpcPort, "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"get_miner_data\"}",
		[this](const char* data, size_t size)
		{
			parse_get_miner_data_rpc(data, size);
		},
		[this](const char* data, size_t size)
		{
			if (size > 0) {
				LOGWARN(1, "get_miner_data RPC request failed: error " << log::const_buf(data, size) << ", trying again in 1 second");
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				get_miner_data();
			}
		});
}

void p2pool::parse_get_miner_data_rpc(const char* data, size_t size)
{
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(data, size);

	if (!doc.IsObject() || !doc.HasMember("result")) {
		LOGWARN(1, "get_miner_data RPC response is invalid, skipping it");
		return;
	}

	MinerData minerData;

	const auto& result = doc["result"];

	if (!PARSE(result, minerData, major_version) ||
		!PARSE(result, minerData, height) ||
		!PARSE(result, minerData, prev_id) ||
		!PARSE(result, minerData, seed_hash) ||
		!PARSE(result, minerData, median_weight) ||
		!PARSE(result, minerData, already_generated_coins) ||
		!PARSE(result, minerData, difficulty)) {
		LOGWARN(1, "get_miner_data RPC response failed to parse, skipping it");
		return;
	}

	auto it = result.FindMember("tx_backlog");

	if ((it != result.MemberEnd()) && it->value.IsArray()) {
		const auto& tx_backlog = it->value.GetArray();
		for (rapidjson::SizeType i = 0, n = tx_backlog.Size(); i < n; ++i) {
			const auto& tx = tx_backlog[i];

			if (!tx.IsObject()) {
				continue;
			}

			TxMempoolData tx_data;
			if (PARSE(tx, tx_data, id) && PARSE(tx, tx_data, weight) && PARSE(tx, tx_data, fee)) {
				tx_data.blob_size = 0;
				minerData.tx_backlog.emplace_back(std::move(tx_data));
			}
		}
	}

	handle_miner_data(minerData);
	download_block_headers(minerData.height);
}

bool p2pool::parse_block_header(const char* data, size_t size, ChainMain& c)
{
	rapidjson::Document doc;
	if (doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(data, size).HasParseError() || !doc.IsObject()) {
		LOGERR(1, "parse_block_header: invalid JSON response from daemon");
		return false;
	}

	auto it = doc.FindMember("result");
	if (it == doc.MemberEnd() || !it->value.IsObject()) {
		LOGERR(1, "parse_block_header: invalid JSON response from daemon : 'result' is not found or not an object");
		return false;
	}

	auto it2 = it->value.FindMember("block_header");
	if (it2 == it->value.MemberEnd() || !it2->value.IsObject()) {
		LOGERR(1, "parse_block_header: invalid JSON response from daemon: 'block_header' is not found or not an object");
		return false;
	}

	const auto& v = it2->value;

	if (!parseValue(v, "difficulty", c.difficulty.lo) || !parseValue(v, "difficulty_top64", c.difficulty.hi)) {
		LOGERR(1, "parse_block_header: invalid JSON response from daemon: failed to parse difficulty");
		return false;
	}

	if (!PARSE(v, c, height) || !PARSE(v, c, timestamp) || !PARSE(v, c, reward) || !parseValue(v, "hash", c.id)) {
		LOGERR(1, "parse_block_header: invalid JSON response from daemon: failed to parse 'block_header'");
		return false;
	}

	{
		WriteLock lock(m_mainchainLock);
		m_mainchainByHeight[c.height] = c;
		m_mainchainByHash[c.id] = c;
	}

	LOGINFO(4, "parsed block header for height " << c.height);
	return true;
}

uint32_t p2pool::parse_block_headers_range(const char* data, size_t size)
{
	rapidjson::Document doc;
	if (doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(data, size).HasParseError() || !doc.IsObject()) {
		LOGERR(1, "parse_block_headers_range: invalid JSON response from daemon");
		return 0;
	}

	auto it = doc.FindMember("result");
	if (it == doc.MemberEnd() || !it->value.IsObject()) {
		LOGERR(1, "parse_block_headers_range: invalid JSON response from daemon: 'result' is not found or not an object");
		return 0;
	}

	auto it2 = it->value.FindMember("headers");
	if (it2 == it->value.MemberEnd() || !it2->value.IsArray()) {
		LOGERR(1, "parse_block_headers_range: invalid JSON response from daemon: 'headers' is not found or not an array");
		return 0;
	}

	uint32_t num_headers_parsed = 0;

	WriteLock lock(m_mainchainLock);

	auto headers = it2->value.GetArray();
	uint64_t min_height = std::numeric_limits<uint64_t>::max();
	uint64_t max_height = 0;
	for (auto i = headers.begin(); i != headers.end(); ++i) {
		if (!i->IsObject()) {
			continue;
		}

		ChainMain c;

		if (!parseValue(*i, "difficulty", c.difficulty.lo) || !parseValue(*i, "difficulty_top64", c.difficulty.hi)) {
			continue;
		}

		if (PARSE(*i, c, height) && PARSE(*i, c, timestamp) && PARSE(*i, c, reward) && parseValue(*i, "hash", c.id)) {
			min_height = std::min(min_height, c.height);
			max_height = std::max(max_height, c.height);
			m_mainchainByHeight[c.height] = c;
			m_mainchainByHash[c.id] = c;
			++num_headers_parsed;
		}
	}

	LOGINFO(4, "parsed " << num_headers_parsed << " block headers for heights " << min_height << " - " << max_height);
	return num_headers_parsed;
}

void p2pool::api_update_network_stats()
{
	if (!m_api) {
		return;
	}

	ChainMain mainnet_tip;
	{
		ReadLock lock(m_mainchainLock);
		mainnet_tip = m_mainchainByHash[m_minerData.prev_id];
	}

	m_api->set(p2pool_api::Category::NETWORK, "stats",
		[mainnet_tip](log::Stream& s)
		{
			s << "{\"difficulty\":" << mainnet_tip.difficulty
				<< ",\"hash\":\"" << mainnet_tip.id
				<< "\",\"height\":" << mainnet_tip.height
				<< ",\"reward\":" << mainnet_tip.reward
				<< ",\"timestamp\":" << mainnet_tip.timestamp << "}";
		});

	api_update_stats_mod();
}

void p2pool::api_update_pool_stats()
{
	if (!m_api) {
		return;
	}

	uint64_t t;
	const difficulty_type& diff = m_sideChain->difficulty();
	const uint64_t hashrate = udiv128(diff.hi, diff.lo, m_sideChain->block_time(), &t);
	const uint64_t miners = std::max<uint64_t>(m_sideChain->miner_count(), m_p2pServer ? m_p2pServer->peer_list_size() : 0U);
	const difficulty_type total_hashes = m_sideChain->total_hashes();

	time_t last_block_found_time = 0;
	uint64_t last_block_found_height = 0;
	uint64_t total_blocks_found = 0;

	{
		MutexLock lock(m_foundBlocksLock);
		if (!m_foundBlocks.empty()) {
			total_blocks_found = m_foundBlocks.size();
			last_block_found_time = m_foundBlocks.back().timestamp;
			last_block_found_height = m_foundBlocks.back().height;
		}
	}

	m_api->set(p2pool_api::Category::POOL, "stats",
		[hashrate, miners, &total_hashes, last_block_found_time, last_block_found_height, total_blocks_found](log::Stream& s)
		{
			s << "{\"pool_list\":[\"pplns\"],\"pool_statistics\":{\"hashRate\":" << hashrate
				<< ",\"miners\":" << miners
				<< ",\"totalHashes\":" << total_hashes
				<< ",\"lastBlockFoundTime\":" << last_block_found_time
				<< ",\"lastBlockFound\":" << last_block_found_height
				<< ",\"totalBlocksFound\":" << total_blocks_found
				<< "}}";
		});

	api_update_stats_mod();
}

void p2pool::api_update_stats_mod()
{
	if (!m_api) {
		return;
	}

	ChainMain mainnet_tip;
	{
		ReadLock lock(m_mainchainLock);
		mainnet_tip = m_mainchainByHash[m_minerData.prev_id];
	}

	time_t last_block_found_time = 0;
	uint64_t last_block_found_height = 0;
	hash last_block_found_hash;
	difficulty_type last_block_total_hashes;

	{
		MutexLock lock(m_foundBlocksLock);
		if (!m_foundBlocks.empty()) {
			last_block_found_time = m_foundBlocks.back().timestamp;
			last_block_found_height = m_foundBlocks.back().height;
			last_block_found_hash = m_foundBlocks.back().id;
			last_block_total_hashes = m_foundBlocks.back().total_hashes;
		}
	}

	char last_block_found_buf[log::Stream::BUF_SIZE + 1];
	log::Stream s(last_block_found_buf);
	s << last_block_found_hash << '\0';
	memcpy(last_block_found_buf + 4, "...", 4);

	const uint64_t miners = std::max<uint64_t>(m_sideChain->miner_count(), m_p2pServer ? m_p2pServer->peer_list_size() : 0U);

	uint64_t t;
	const difficulty_type& diff = m_sideChain->difficulty();
	const uint64_t hashrate = udiv128(diff.hi, diff.lo, m_sideChain->block_time(), &t);

	const difficulty_type total_hashes = m_sideChain->total_hashes();
	if (total_hashes < last_block_total_hashes) {
		return;
	}

	const uint64_t round_hashes = total_hashes.lo - last_block_total_hashes.lo;
	const int stratum_port = DEFAULT_STRATUM_PORT;

	m_api->set(p2pool_api::Category::GLOBAL, "stats_mod",
		[&mainnet_tip, last_block_found_time, &last_block_found_buf, last_block_found_height, miners, hashrate, round_hashes, stratum_port](log::Stream& s)
		{
			s << "{\"config\":{\"ports\":[{\"port\":" << stratum_port << ",\"tls\":false}],\"fee\":0,\"minPaymentThreshold\":400000000},\"network\":{\"height\":"
				<< mainnet_tip.height << "},\"pool\":{\"stats\":{\"lastBlockFound\":\""
				<< last_block_found_time << "000\"},\"blocks\":[\""
				<< static_cast<char*>(last_block_found_buf) << static_cast<char*>(last_block_found_buf) + HASH_SIZE * 2 - 4 << ':'
				<< last_block_found_time << "\",\""
				<< last_block_found_height << "\"],\"miners\":"
				<< miners << ",\"hashrate\":"
				<< hashrate << ",\"roundHashes\":"
				<< round_hashes << "}}";
		});
}

void p2pool::cleanup_mainchain_data(uint64_t height)
{
	// Expects m_mainchainLock to be already locked here
	// Deletes everything older than 720 blocks, except for the 3 latest RandomX seed heights

	constexpr uint64_t PRUNE_DISTANCE = BLOCK_HEADERS_REQUIRED;
	const uint64_t seed_height = get_seed_height(height);
	const std::array<uint64_t, 3> seed_heights{ seed_height, seed_height - SEEDHASH_EPOCH_BLOCKS, seed_height - SEEDHASH_EPOCH_BLOCKS * 2 };

	for (auto it = m_mainchainByHeight.begin(); it != m_mainchainByHeight.end();) {
		const uint64_t h = it->first;
		if (h + PRUNE_DISTANCE >= height) {
			break;
		}

		if (std::find(seed_heights.begin(), seed_heights.end(), h) == seed_heights.end()) {
			m_mainchainByHash.erase(it->second.id);
			it = m_mainchainByHeight.erase(it);
		}
		else {
			++it;
		}
	}
}

void p2pool::api_update_block_found(const ChainMain* data)
{
	clear_crypto_cache();

	if (!m_api) {
		return;
	}

	const time_t cur_time = time(nullptr);
	const difficulty_type total_hashes = m_sideChain->total_hashes();
	difficulty_type diff;

	if (data && get_difficulty_at_height(data->height, diff)) {
		std::ofstream f(FOUND_BLOCKS_FILE, std::ios::app);
		if (f.is_open()) {
			f << cur_time << ' ' << data->height << ' ' << data->id << ' ' << diff << ' ' << total_hashes << '\n';
		}
	}

	std::vector<FoundBlock> found_blocks;
	{
		MutexLock lock(m_foundBlocksLock);
		if (data) {
			m_foundBlocks.emplace_back(cur_time, data->height, data->id, diff, total_hashes);
		}
		found_blocks.assign(m_foundBlocks.end() - std::min<size_t>(m_foundBlocks.size(), 51), m_foundBlocks.end());
	}

	m_api->set(p2pool_api::Category::POOL, "blocks",
		[&found_blocks](log::Stream& s)
		{
			s << '[';
			bool first = true;
			for (auto i = found_blocks.rbegin(); i != found_blocks.rend(); ++i) {
				if (!first) {
					s << ',';
				}
				s << "{\"height\":" << i->height << ','
					<< "\"hash\":\"" << i->id << "\","
					<< "\"difficulty\":" << i->block_diff << ','
					<< "\"totalHashes\":" << i->total_hashes << ','
					<< "\"ts\":" << i->timestamp << '}';
				first = false;
			}
			s << ']';
		});

	api_update_stats_mod();
}

bool p2pool::get_difficulty_at_height(uint64_t height, difficulty_type& diff)
{
	ReadLock lock(m_mainchainLock);

	auto it = m_mainchainByHeight.find(height);
	if (it == m_mainchainByHeight.end()) {
		return false;
	}

	diff = it->second.difficulty;
	return true;
}

static void on_signal(uv_signal_t* handle, int signum)
{
	p2pool* pool = reinterpret_cast<p2pool*>(handle->data);

	switch (signum) {
	case SIGHUP:
		LOGINFO(1, "caught SIGHUP");
		break;
	case SIGINT:
		LOGINFO(1, "caught SIGINT");
		break;
	case SIGTERM:
		LOGINFO(1, "caught SIGTERM");
		break;
#ifdef SIGBREAK
	case SIGBREAK:
		LOGINFO(1, "caught SIGBREAK");
		break;
#endif
#ifdef SIGUSR1
	case SIGUSR1:
		log::reopen();
		return;
#endif
	default:
		LOGINFO(1, "caught signal " << signum);
	}

	LOGINFO(1, "stopping");

	uv_signal_stop(handle);
	pool->stop();
}

static bool init_uv_threadpool()
{
	static uv_work_t dummy;
	return (uv_queue_work(uv_default_loop_checked(), &dummy, [](uv_work_t*) {}, nullptr) == 0);
}

static bool init_signals(p2pool* pool)
{
	constexpr int signal_names[] = {
		SIGHUP,
		SIGINT,
		SIGTERM,
#ifdef SIGBREAK
		SIGBREAK,
#endif
#ifdef SIGUSR1
		SIGUSR1,
#endif
	};

	static uv_signal_t signals[array_size(signal_names)];

	for (size_t i = 0; i < array_size(signal_names); ++i) {
		uv_signal_init(uv_default_loop_checked(), &signals[i]);
		signals[i].data = pool;
		const int rc = uv_signal_start(&signals[i], on_signal, signal_names[i]);
		if (rc != 0) {
			LOGERR(1, "failed to initialize signal, error " << rc);
			return false;
		}
	}

	return true;
}

void p2pool::stop()
{
	uv_async_send(&m_stopAsync);
}

int p2pool::run()
{
	if (!m_params->ok()) {
		LOGERR(1, "Invalid or missing command line. Try \"p2pool --help\".");
		return 1;
	}

	if (!init_uv_threadpool()) {
		LOGERR(1, "failed to start UV thread pool");
		return 1;
	}

	if (!init_signals(this)) {
		LOGERR(1, "failed to initialize signal handlers");
		return 1;
	}

	try {
		get_info();
		load_found_blocks();
		const int rc = uv_run(uv_default_loop_checked(), UV_RUN_DEFAULT);
		LOGINFO(1, "uv_run exited, result = " << rc);
		delete m_ZMQReader;
	}
	catch (const std::exception& e) {
		const char* s = e.what();
		LOGERR(1, "exception " << s);
		panic();
	}

	m_stopped = true;

	bkg_jobs_tracker.wait();

	delete m_stratumServer;
	delete m_p2pServer;

	LOGINFO(1, "stopped");
	return 0;
}

} // namespace p2pool
