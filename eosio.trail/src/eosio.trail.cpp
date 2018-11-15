#include <include/eosio.trail.hpp>

trail::trail(name self, name code, datastream<const char*> ds) : contract(self, code, ds), environment(self, self.value) {
    if (!environment.exists()) {

        env_struct = env{
            self, //publisher
            0, //total_tokens
            0, //total_voters
            0, //total_proposals
            0, //total_elections
            asset(0, symbol("VOTE", 4)), //vote_supply //TODO: remove?
            now() //time_now
        };

        environment.set(env_struct, self);
    } else {
        env_struct = environment.get();
        env_struct.time_now = now();
    }
}

trail::~trail() {
    if (environment.exists()) {
        environment.set(env_struct, env_struct.publisher);
    }
}

#pragma region Token_Actions

void trail::regtoken(asset native, name publisher) {
    require_auth(publisher);

    auto sym = native.symbol.raw();

    stats statstable(name("eosio.token"), sym);
    auto eosio_existing = statstable.find(sym);
    eosio_assert(eosio_existing == statstable.end(), "Token with symbol already exists in eosio.token" );

    registries_table registries(_self, _self.value);
    auto r = registries.find(sym);
    eosio_assert(r == registries.end(), "Token Registry with that symbol already exists in Trail");

    //TODO: assert only 1 token per publisher

    registries.emplace(publisher, [&]( auto& a ){
        a.native = native;
        a.publisher = publisher;
    });

    env_struct.total_tokens++;

    print("\nToken Registration: SUCCESS");
}

void trail::unregtoken(asset native, name publisher) {
    require_auth(publisher);
    
    auto sym = native.symbol.raw();
    registries_table registries(_self, _self.value);
    auto r = registries.find(sym);

    eosio_assert(r != registries.end(), "Token Registry does not exist.");

    registries.erase(r);

    env_struct.total_tokens--;

    print("\nToken Unregistration: SUCCESS");
}

#pragma endregion Token_Actions

#pragma region Voting_Actions

void trail::regvoter(name voter) {
    require_auth(voter);

    voters_table voters(_self, _self.value);
    auto v = voters.find(voter.value);

    eosio_assert(v == voters.end(), "Voter already exists");

    voters.emplace(voter, [&]( auto& a ){
        a.voter = voter;
        a.votes = asset(0, symbol("VOTE", 4));
    });

    env_struct.total_voters++;

    print("\nVoterID Registration: SUCCESS");
}

void trail::unregvoter(name voter) {
    require_auth(voter);

    voters_table voters(_self, _self.value);
    auto v = voters.find(voter.value);

    eosio_assert(v != voters.end(), "Voter Doesn't Exist");

    auto vid = *v;

    env_struct.vote_supply -= vid.votes;

    voters.erase(v);

    env_struct.total_voters--;

    print("\nVoterID Unregistration: SUCCESS");
}

void trail::regballot(name publisher, uint8_t ballot_type, symbol voting_symbol, uint32_t begin_time, uint32_t end_time, string info_url) {
    require_auth(publisher);
    eosio_assert(ballot_type >= 0 && ballot_type <= 1, "invalid ballot type"); //NOTE: update valid range as new ballot types are added

    //TODO: check for voting_token existence?

    uint64_t new_ref_id;

    switch (ballot_type) {
        case 0 : 
            new_ref_id = makeproposal(publisher, voting_symbol, begin_time, end_time, info_url);
            env_struct.total_proposals++; 
            break;
        case 1 : 
            new_ref_id = makeelection(publisher, voting_symbol, begin_time, end_time, info_url);
            env_struct.total_elections++;
            break;
    }

    ballots_table ballots(_self, _self.value);

    ballots.emplace(publisher, [&]( auto& a ) {
        a.ballot_id = ballots.available_primary_key();
        a.table_id = ballot_type;
        a.reference_id = new_ref_id;
    });

}

void trail::unregballot(name publisher, uint64_t ballot_id) {
    require_auth(publisher);

    ballots_table ballots(_self, _self.value);
    auto b = ballots.find(ballot_id);
    eosio_assert(b != ballots.end(), "Ballot Doesn't Exist");
    auto bal = *b;

    bool del_success = false;

    switch (bal.table_id) {
        case 0 : 
            del_success = deleteproposal(bal.reference_id);
            env_struct.total_proposals--;
            break;
        case 1 : 
            del_success = deleteelection(bal.reference_id);
            env_struct.total_elections--;
            break;
    }

    if (del_success) {
        ballots.erase(b);
    }

}

void trail::mirrorstake(name voter, uint32_t lock_period) {
    require_auth(voter);
    eosio_assert(lock_period >= MIN_LOCK_PERIOD, "lock period must be greater than 1 day (86400 secs)");
    eosio_assert(lock_period <= MAX_LOCK_PERIOD, "lock period must be less than 3 months (7,776,000 secs)");

    asset max_votes = get_liquid_tlos(voter) + get_staked_tlos(voter);
    eosio_assert(max_votes.symbol == symbol("TLOS", 4), "only TLOS can be used to get VOTEs"); //NOTE: redundant?
    eosio_assert(max_votes > asset(0, symbol("TLOS", 4)), "must get a positive amount of VOTEs"); //NOTE: redundant?
    
    voters_table voters(_self, _self.value);
    auto v = voters.find(voter.value);
    eosio_assert(v != voters.end(), "voter is not registered");

    auto vid = *v;
    eosio_assert(now() >= vid.release_time, "cannot get more votes until lock period is over");

    votelevies_table votelevies(_self, _self.value);
    auto vl = votelevies.find(voter.value);

    auto new_votes = asset(max_votes.amount, symbol("VOTE", 4)); //mirroring TLOS amount, not spending/locking it up
    asset decay_amount = calc_decay(voter, new_votes);
    
    if (vl != votelevies.end()) { //NOTE: if no levy found, give levy of 0
        auto levy = *vl;
        asset new_levy = (levy.levy_amount - decay_amount); //subtracting levy

        if (new_levy < asset(0, symbol("VOTE", 4))) {
            new_levy = asset(0, symbol("VOTE", 4));
        }

        new_votes -= new_levy;

        votelevies.modify(vl, same_payer, [&]( auto& a ) {
            a.levy_amount = new_levy;
        });
    }

    if (new_votes < asset(0, symbol("VOTE", 4))) { //NOTE: can't have less than 0 votes
        new_votes = asset(0, symbol("VOTE", 4));
    }

    voters.modify(v, same_payer, [&]( auto& a ) {
        a.votes = new_votes;
        a.release_time = now() + lock_period;
    });

    print("\nRegister For Votes: SUCCESS");
}

void trail::castvote(name voter, uint64_t ballot_id, uint16_t direction) {
    require_auth(voter);
    eosio_assert(direction >= uint16_t(0) && direction <= uint16_t(2), "Invalid Vote. [0 = NO, 1 = YES, 2 = ABSTAIN]");

    voters_table voters(_self, _self.value);
    auto v = voters.find(voter.value);
    eosio_assert(v != voters.end(), "voter is not registered");

    auto vid = *v;

    ballots_table ballots(_self, _self.value);
    auto b = ballots.find(ballot_id);
    eosio_assert(b != ballots.end(), "ballot with given ballot_id doesn't exist");
    auto bal = *b;

    bool vote_success = false;

    switch (bal.table_id) {
        case 0 : 
            vote_success = voteforproposal(voter, ballot_id, bal.reference_id, direction);
            break;
        case 1 : 
            //vote_success = voteforelection(voter, ballot_id, bal.reference_id, direction);
            break;
    }

}

void trail::closeballot(name publisher, uint64_t ballot_id, uint8_t pass) {
    require_auth(publisher);

    ballots_table ballots(_self, _self.value);
    auto b = ballots.find(ballot_id);
    eosio_assert(b != ballots.end(), "ballot with given ballot_id doesn't exist");
    auto bal = *b;

    bool close_success = false;

    switch (bal.table_id) {
        case 0 : 
            close_success = closeproposal(bal.reference_id, pass);
            break;
        case 1 : 
            //close_success = voteforelection(voter, ballot_id, bal.reference_id, direction);
            break;
    }

    

}

void trail::nextcycle(name publisher, uint64_t ballot_id, uint32_t new_begin_time, uint32_t new_end_time) {
    require_auth(publisher);

    ballots_table ballots(_self, _self.value);
    auto b = ballots.find(ballot_id);
    eosio_assert(b != ballots.end(), "Ballot Doesn't Exist");
    auto bal = *b;

    eosio_assert(bal.table_id == 0, "ballot type doesn't support cycles");

    proposals_table proposals(_self, _self.value);
    auto p = proposals.find(bal.reference_id);
    eosio_assert(p != proposals.end(), "proposal doesn't exist");
    auto prop = *p;

    auto sym = prop.no_count.symbol; //NOTE: uses same voting symbol as before

    proposals.modify(p, same_payer, [&]( auto& a ) {
        a.no_count = asset(0, sym);
        a.yes_count = asset(0, sym);
        a.abstain_count = asset(0, sym);
        a.unique_voters = uint32_t(0);
        a.begin_time = new_begin_time;
        a.end_time = new_end_time;
        a.status = false;
    });

}

void trail::deloldvotes(name voter, uint16_t num_to_delete) {
    require_auth(voter);

    votereceipts_table votereceipts(_self, voter.value);
    auto itr = votereceipts.begin();

    while (itr != votereceipts.end() && num_to_delete > 0) {
        if (itr->expiration < env_struct.time_now) { //NOTE: votereceipt has expired
            itr = votereceipts.erase(itr); //NOTE: returns iterator to next element
            num_to_delete--;
        } else {
            itr++;
        }
    }

}

#pragma endregion Voting_Actions



#pragma region Helper_Functions

uint64_t trail::makeproposal(name publisher, symbol voting_symbol, uint32_t begin_time, uint32_t end_time, string info_url) {

    proposals_table proposals(_self, _self.value);

    uint64_t new_prop_id = proposals.available_primary_key();

    proposals.emplace(publisher, [&]( auto& a ) {
        a.prop_id = new_prop_id
        a.publisher = publisher;
        a.info_url = info_url;
        a.no_count = asset(0, voting_symbol);
        a.yes_count = asset(0, voting_symbol);
        a.abstain_count = asset(0, voting_symbol);
        a.unique_voters = uint32_t(0);
        a.begin_time = begin_time;
        a.end_time = end_time;
    });

    print("\nProposal Creation: SUCCESS");

    return new_prop_id;
}

bool trail::deleteproposal(uint64_t prop_id) {
    proposals_table proposals(_self, _self.value);
    auto p = proposals.find(prop_id);
    eosio_assert(p != proposals.end(), "proposal doesn't exist");
    auto prop = *p;
    eosio_assert(now() < prop.begin_time, "cannot delete proposal once voting has begun");

    proposals.erase(p);

    print("\nProposal Deletion: SUCCESS");

    return true;
}

bool trail::voteforproposal(name voter, uint64_t ballot_id, uint64_t prop_id, uint16_t direction) {
    
    proposals_table proposals(_self, _self.value);
    auto p = proposals.find(prop_id);
    eosio_assert(p != proposals.end(), "proposal doesn't exist");
    auto prop = *p;

    eosio_assert(env_struct.time_now >= prop.begin_time && env_struct.time_now <= prop.end_time, "ballot voting window not open");
    //eosio_assert(vid.release_time >= bal.end_time, "can only vote for ballots that end before your lock period is over...prevents double voting!");

    votereceipts_table votereceipts(_self, voter.value);
    auto vr_itr = votereceipts.find(ballot_id);
    //eosio_assert(vr_itr == votereceipts.end(), "voter has already cast vote for this ballot");
    
    uint32_t new_voter = 1;
    asset vote_weight = get_vote_weight(voter, prop.no_count.symbol);

    if (vr_itr == votereceipts.end()) { //NOTE: voter hasn't voted on ballot before
        votereceipts.emplace(voter, [&]( auto& a ){
            a.ballot_id = ballot_id;
            a.direction = direction;
            a.weight = vote_weight;
            a.expiration = bal.end_time;
        });
    } else { //NOTE: vote for ballot_id already exists
        auto vr = *vr_itr;

        if (vr.expiration == prop.end_time && vr.direction != direction) { //NOTE: vote different and for same cycle

            switch (vr.direction) { //NOTE: remove old vote weight
                case 0 : prop.no_count -= vr.weight; break;
                case 1 : prop.yes_count -= vr.weight; break;
                case 2 : prop.abstain_count -= vr.weight; break;
            }

            votereceipts.modify(vr_itr, same_payer, [&]( auto& a ) {
                a.direction = direction;
                a.weight = vote_weight;
            });

            new_voter = 0;

            print("\nVote Recast: SUCCESS");
        }// else if (vr.expiration < prop.end_time) { //NOTE: vote for new cycle
        //     votereceipts.modify(vr_itr, same_payer, [&]( auto& a ) {
        //         a.direction = direction;
        //         a.weight = vote_weight;
        //         a.expiration = bal.end_time;
        //     });
        // }
    }

    switch (direction) {
        case 0 : prop.no_count += vote_weight; break;
        case 1 : prop.yes_count += vote_weight; break;
        case 2 : prop.abstain_count += vote_weight; break;
    }

    proposals.modify(p, same_payer, [&]( auto& a ) {
        a.no_count = prop.no_count;
        a.yes_count = prop.yes_count;
        a.abstain_count = prop.abstain_count;
        a.unique_voters += new_voter;
    });
}

bool trail::closeproposal(uint64_t prop_id, uint8_t pass) {
    proposals_table proposals(_self, _self.value);
    auto p = proposals.find(prop_id);
    eosio_assert(p != proposals.end(), "proposal doesn't exist");
    auto prop = *p;

    eosio_assert(now() > prop.end_time, "can't close proposal while voting is still open");

    proposals.modify(p, same_payer, [&]( auto& a ) {
        a.status = pass;
    });

    return true;
}



uint64_t trail::makeelection(name publisher, symbol voting_symbol, uint32_t begin_time, uint32_t end_time, string info_url) {
    elections_table elections(_self, _self.value);

    uint64_t new_elec_id = elections.available_primary_key();
    vector<candidate> empty_candidate_list;

    elections.emplace(publisher, [&]( auto& a ) {
        a.election_id = new_elec_id;
        a.publisher = publisher;
        a.election_info = info_url;
        a.candidates = empty_candidate_list;
        a.unique_voters = 0;
        a.voting_symbol = voting_symbol;
        a.begin_time = begin_time;
        a.end_time = end_time;
    });

    print("\nElection Creation: SUCCESS");

    return new_elec_id;
}

bool trail::deleteelection(uint64_t elec_id) {
    elections_table elections(_self, _self.value);
    auto e = elections.find(elec_id);
    eosio_assert(e != elections.end(), "proposal doesn't exist");
    auto elec = *e;
    eosio_assert(now() < elec.begin_time, "cannot delete election once voting has begun");

    elections.erase(e);

    print("\nElection Deletion: SUCCESS");

    return true;
}



asset trail::get_vote_weight(name voter, symbol voting_token) {

    // switch (voting_token.raw()) {
    //     case symbol("VOTE", 0).raw() :
    //         voters_table voters(_self, _self.value);
    //         auto v = voters.find(voter.value);
    //         eosio_assert(v != voters.end(), "voter is not registered");
    //         break;
    //     case symbol("TFVT", 0).raw() :
    //         break;
    // }

    if (voting_token == symbol("VOTE", 0)) {
        voters_table voters(_self, _self.value);
        auto v = voters.find(voter.value);
        eosio_assert(v != voters.end(), "voter is not registered");
        auto vid = *v;

        return vid.votes;
    } else if (voting_token == symbol("TFVT", 0)) {
        //TODO: implement TFVT
    }

}

#pragma endregion Helper_Functions



#pragma region Reactions

void trail::update_from_levy(name from, asset amount) {
    votelevies_table fromlevies(_self, _self.value);
    auto vl_from_itr = fromlevies.find(from.value);
    
    if (vl_from_itr == fromlevies.end()) {
        fromlevies.emplace(_self, [&]( auto& a ){
            a.voter = from;
            a.levy_amount = asset(0, symbol("VOTE", 4));
            a.last_decay = env_struct.time_now;
        });
    } else {
        auto vl_from = *vl_from_itr;
        asset new_levy = vl_from.levy_amount - amount;

        if (new_levy < asset(0, symbol("VOTE", 4))) {
            new_levy = asset(0, symbol("VOTE", 4));
        }

        fromlevies.modify(vl_from_itr, same_payer, [&]( auto& a ) {
            a.levy_amount = new_levy;
            a.last_decay = env_struct.time_now;
        });
    }
}

void trail::update_to_levy(name to, asset amount) {
    votelevies_table tolevies(_self, _self.value);
    auto vl_to_itr = tolevies.find(to.value);

    if (vl_to_itr == tolevies.end()) {
        tolevies.emplace(_self, [&]( auto& a ){
            a.voter = to;
            a.levy_amount = asset(0, symbol("VOTE", 4));
            a.last_decay = env_struct.time_now;
        });
    } else {
        auto vl_to = *vl_to_itr;
        asset new_levy = vl_to.levy_amount + amount;

        tolevies.modify(vl_to_itr, same_payer, [&]( auto& a ) {
            a.levy_amount = new_levy;
            a.last_decay = env_struct.time_now;
        });
    }
}

asset trail::calc_decay(name voter, asset amount) {
    votelevies_table votelevies(_self, _self.value);
    auto vl_itr = votelevies.find(voter.value);

    uint32_t time_delta;

    if (vl_itr != votelevies.end()) {
        auto vl = *vl_itr;
        time_delta = env_struct.time_now - vl.last_decay;
        return asset(int64_t(time_delta / 120), symbol("VOTE", 4));
    }

    return asset(0, symbol("VOTE", 4));
}

#pragma endregion Reactions

//EOSIO_DISPATCH(trail, )

extern "C" {
    void apply(uint64_t self, uint64_t code, uint64_t action) {

        size_t size = action_data_size();
        constexpr size_t max_stack_buffer_size = 512;
        void* buffer = nullptr;
        if( size > 0 ) {
            buffer = max_stack_buffer_size < size ? malloc(size) : alloca(size);
            read_action_data(buffer, size);
        }
        datastream<const char*> ds((char*)buffer, size);

        trail trailservice(name(self), name(code), ds);

        if(code == self && action == name("regtoken").value) {
            execute_action(name(self), name(code), &trail::regtoken);
        } else if (code == self && action == name("unregtoken").value) {
            execute_action(name(self), name(code), &trail::unregtoken);
        } else if (code == self && action == name("regvoter").value) {
            execute_action(name(self), name(code), &trail::regvoter);
        } else if (code == self && action == name("unregvoter").value) {
            execute_action(name(self), name(code), &trail::unregvoter);
        } else if (code == self && action == name("regballot").value) {
            execute_action(name(self), name(code), &trail::regballot);
        } else if (code == self && action == name("unregballot").value) {
            execute_action(name(self), name(code), &trail::unregballot);
        } else if (code == self && action == name("mirrorstake").value) {
            execute_action(name(self), name(code), &trail::mirrorstake);
        } else if (code == self && action == name("castvote").value) {
            execute_action(name(self), name(code), &trail::castvote);
        } else if (code == self && action == name("closeballot").value) {
            execute_action(name(self), name(code), &trail::closeballot);
        } else if (code == self && action == name("nextcycle").value) {
            execute_action(name(self), name(code), &trail::nextcycle);
        } else if (code == self && action == name("deloldvotes").value) {
            execute_action(name(self), name(code), &trail::deloldvotes);
        } else if (code == name("eosio.token").value && action == name("transfer").value) { //NOTE: updates vote_levy after transfers
            auto args = unpack_action_data<transfer_args>();
            trailservice.update_from_levy(args.from, asset(args.quantity.amount, symbol("VOTE", 4)));
            trailservice.update_to_levy(args.to, asset(args.quantity.amount, symbol("VOTE", 4)));
        }
    } //end apply
}; //end dispatcher