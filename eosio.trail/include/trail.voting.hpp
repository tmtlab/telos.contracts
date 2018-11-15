/**
 * This file includes all definitions necessary to interact with Trail's voting system. Developers who want to
 * utilize the system simply must include this file in their implementation to interact with the information
 * stored by Trail.
 * 
 * @author Craig Branscom
 */

#include <eosiolib/eosio.hpp>
#include <eosiolib/permission.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/action.hpp>
#include <eosiolib/singleton.hpp>

using namespace std;
using namespace eosio;

#pragma region Structs

struct [[eosio::table]] vote_receipt {
    uint64_t ballot_id;
    uint16_t direction;
    asset weight;
    uint32_t expiration;

    uint64_t primary_key() const { return ballot_id; }
    EOSLIB_SERIALIZE(vote_receipt, (ballot_id)(direction)(weight)(expiration))
};

struct [[eosio::table]] vote_levy {
    name voter;
    asset levy_amount;
    uint32_t last_decay;

    uint64_t primary_key() const { return voter.value; }
    EOSLIB_SERIALIZE(vote_levy, (voter)(levy_amount)(last_decay))
};

struct [[eosio::table]] voter_id {
    name voter;
    asset votes;
    uint32_t release_time;

    uint64_t primary_key() const { return voter.value; }
    EOSLIB_SERIALIZE(voter_id, (voter)(votes)(release_time))
};

struct [[eosio::table]] ballot {
    uint64_t ballot_id;
    uint8_t table_id;
    uint64_t reference_id;

    uint64_t primary_key() const { return ballot_id; }
    EOSLIB_SERIALIZE(ballot, (ballot_id)(table_id)(reference_id))
};

struct [[eosio::table]] proposal {
    uint64_t prop_id;
    name publisher;
    string info_url;
    
    asset no_count;
    asset yes_count;
    asset abstain_count;
    uint32_t unique_voters;

    uint32_t begin_time;
    uint32_t end_time;
    uint8_t status; // 0 = OPEN, 1 = PASS, 2 = FAIL

    uint64_t primary_key() const { return prop_id; }
    EOSLIB_SERIALIZE(proposal, (prop_id)(publisher)(info_url)
        (no_count)(yes_count)(abstain_count)(unique_voters)
        (begin_time)(end_time)(status))
};

struct candidate {
    name member;
    string info_link;
    asset votes;
    uint8_t status;
};

struct [[eosio::table]] election {
    uint64_t election_id;
    name publisher;
    string election_info;

    vector<candidate> candidates;
    uint32_t unique_voters;
    symbol voting_symbol;
    
    uint32_t begin_time;
    uint32_t end_time;

    uint64_t primary_key() const { return election_id; }
    EOSLIB_SERIALIZE(election, (election_id)(publisher)
        (candidates)(unique_voters)(voting_symbol)
        (begin_time)(end_time))
};

struct [[eosio::table]] env {
    name publisher;
    
    uint64_t total_tokens;
    uint64_t total_voters;
    uint64_t total_proposals;
    uint64_t total_elections;

    asset vote_supply;

    uint32_t time_now;

    uint64_t primary_key() const { return publisher.value; }
    EOSLIB_SERIALIZE(env, (publisher)
        (total_tokens)(total_voters)(total_proposals)(total_elections)
        (vote_supply)
        (time_now))
};

#pragma endregion Structs

#pragma region Tables

typedef multi_index<name("voters"), voter_id> voters_table;

typedef multi_index<name("ballots"), ballot> ballots_table;

    typedef multi_index<name("proposals"), proposal> proposals_table;

    typedef multi_index<name("elections"), election> elections_table;

typedef multi_index<name("votereceipts"), vote_receipt> votereceipts_table;

typedef multi_index<name("votelevies"), vote_levy> votelevies_table;

typedef singleton<name("environment"), env> environment_singleton;

#pragma endregion Tables

#pragma region Helper_Functions

bool is_voter(name voter) {
    voters_table voters(name("eosio.trail"), name("eosio.trail").value);
    auto v = voters.find(voter.value);

    if (v != voters.end()) {
        return true;
    }

    return false;
}

bool is_ballot(uint64_t ballot_id) {
    ballots_table ballots(name("eosio.trail"), name("eosio.trail").value);
    auto b = ballots.find(ballot_id);

    if (b != ballots.end()) {
        return true;
    }

    return false;
}

// bool is_ballot_publisher(name publisher, uint64_t ballot_id) {
//     ballots_table ballots(name("eosio.trail"), name("eosio.trail").value);
//     auto b = ballots.find(ballot_id);

//     if (b != ballots.end()) {
//         auto bal = *b;

//         if (bal.publisher == publisher) {
//             return true;
//         }
//     }

//     return false;
// }

#pragma endregion Helper_Functions