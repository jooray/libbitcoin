#include "leveldb_chain_keeper.hpp"

#include <bitcoin/utility/logger.hpp>
#include <bitcoin/format.hpp>
#include <bitcoin/transaction.hpp>

namespace libbitcoin {

leveldb_chain_keeper::leveldb_chain_keeper(leveldb_common_ptr common,
    leveldb::DB* db_blocks, leveldb::DB* db_blocks_hash,
    leveldb::DB* db_txs, leveldb::DB* db_spends, leveldb::DB* db_address)
  : common_(common), db_blocks_(db_blocks), db_blocks_hash_(db_blocks_hash),
    db_txs_(db_txs), db_spends_(db_spends), db_address_(db_address)
{
}

void leveldb_chain_keeper::start()
{
}
void leveldb_chain_keeper::stop()
{
}

void leveldb_chain_keeper::add(block_detail_ptr incoming_block)
{
    uint32_t last_block_depth = common_->find_last_block_depth();
    const block_type& actual_block = incoming_block->actual();
    if (!common_->save_block(last_block_depth + 1, actual_block))
        log_fatal(LOG_BLOCKCHAIN) << "Saving block in organizer failed";
}

int leveldb_chain_keeper::find_index(const hash_digest& search_block_hash)
{
    uint32_t depth = common_->get_block_depth(search_block_hash);
    if (depth == std::numeric_limits<uint32_t>::max())
        return -1;
    return static_cast<int>(depth);
}

big_number leveldb_chain_keeper::end_slice_difficulty(size_t slice_begin_index)
{
    big_number total_work = 0;
    leveldb_iterator it(db_blocks_->NewIterator(leveldb::ReadOptions()));
    data_chunk raw_depth = uncast_type(slice_begin_index);
    for (it->Seek(slice(raw_depth)); it->Valid(); it->Next())
    {
        constexpr size_t bits_offset = 4 + 2 * hash_digest_size + 4;
        BITCOIN_ASSERT(it->value().size() >= 84);
        // Deserialize only the bits field of block header.
        std::string raw_bits(it->value().data(), 4);
        auto deserial = make_deserializer(raw_bits.begin(), raw_bits.end());
        uint32_t bits = deserial.read_4_bytes();
        // Accumulate the total work.
        total_work += block_work(bits);
    }
    return total_work;
}

block_detail_ptr reconstruct_block(
    leveldb_common_ptr common, const std::string& value)
{
    leveldb_block_info blk;
    if (!common->deserialize_block(blk, value, true, true))
        return nullptr;
    block_detail_ptr sliced_block = std::make_shared<block_detail>(blk.header);
    for (const hash_digest& tx_hash: blk.tx_hashes)
    {
        // Get the actual transaction.
        leveldb_tx_info tx;
        if (!common->get_transaction(tx, tx_hash, false, true))
            return nullptr;
        sliced_block->actual_ptr()->transactions.push_back(tx.tx);
    }
    return sliced_block;
}

bool leveldb_chain_keeper::end_slice(size_t slice_begin_index,
    block_detail_list& sliced_blocks)
{
    leveldb::WriteBatch blk_batch, blk_hash_batch;
    leveldb_transaction_batch tx_batch;
    leveldb_iterator it(db_blocks_->NewIterator(leveldb::ReadOptions()));
    data_chunk raw_depth = uncast_type(slice_begin_index);
    for (it->Seek(slice(raw_depth)); it->Valid(); it->Next())
    {
        block_detail_ptr sliced_block =
            reconstruct_block(common_, it->value().ToString());
        if (!sliced_block)
            return false;
        // Add to list of sliced blocks
        sliced_blocks.push_back(sliced_block);
        // Make sure to delete hash secondary index too.
        const hash_digest& block_hash = sliced_block->hash();
        // Delete block header...
        blk_batch.Delete(it->key());
        // And it's secondary index.
        blk_hash_batch.Delete(slice_block_hash(block_hash));
        // Remove txs + spends + addresses too
        const auto& transactions = sliced_block->actual().transactions;
        for (const transaction_type& block_tx: transactions)
            if (!clear_transaction_data(tx_batch, block_tx))
                return false;
    }
    leveldb::WriteOptions options;
    // Execute batches.
    db_blocks_->Write(options, &blk_batch);
    db_blocks_hash_->Write(options, &blk_hash_batch);
    db_txs_->Write(options, &tx_batch.tx_batch);
    db_spends_->Write(options, &tx_batch.spends_batch);
    db_address_->Write(options, &tx_batch.address_batch);
    return true;
}

bool leveldb_chain_keeper::clear_transaction_data(
    leveldb_transaction_batch& batch, const transaction_type& remove_tx)
{
    const hash_digest& tx_hash = hash_transaction(remove_tx);
    batch.tx_batch.Delete(slice(tx_hash));
    // Remove spends
    // ... spends don't exist for coinbase txs.
    if (!is_coinbase(remove_tx))
        for (uint32_t input_index = 0; input_index < remove_tx.inputs.size();
            ++input_index)
        {
            const transaction_input_type& input =
                remove_tx.inputs[input_index];
            // We could check if the spend matches the inpoint for safety.
            //const input_point inpoint{tx_hash, input_index};
            // Recreate the key...
            data_chunk spent_key = create_spent_key(input.previous_output);
            // ... Perform the delete.
            batch.spends_batch.Delete(slice(spent_key));
        }
    // Remove addresses
    for (uint32_t output_index = 0; output_index < remove_tx.outputs.size();
        ++output_index)
    {
        const transaction_output_type& output =
            remove_tx.outputs[output_index];
        if (!remove_address(batch.address_batch,
                output.output_script, {tx_hash, output_index}))
            return false;
    }
    return true;
}

bool leveldb_chain_keeper::remove_address(leveldb::WriteBatch& batch,
    const script& output_script, const output_point& outpoint)
{
    data_chunk raw_address = create_address_key(output_script);
    if (raw_address.empty())
        return true;
    data_chunk outpoint_value = create_spent_key(outpoint);
    bool is_found = false;
    leveldb_iterator it(address_iterator(db_address_, raw_address));
    for (; valid_address_iterator(it, raw_address); it->Next())
    {
        if (slice_to_output_point(it->value()) != outpoint)
            continue;
        // We found the address entry we were looking for.
        BITCOIN_ASSERT(!is_found);
        is_found = true;
        // Put changes into batch finally.
        batch.Delete(it->key());
        break;
    }
    BITCOIN_ASSERT(it->status().ok());
    if (!is_found)
        return false;
    return true;
}

} // namespace libbitcoin

