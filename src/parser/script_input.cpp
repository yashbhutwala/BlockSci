//
//  script_input.cpp
//  blocksci
//
//  Created by Harry Kalodner on 3/18/17.
//
//

#define BLOCKSCI_WITHOUT_SINGLETON

#include "script_input.hpp"
#include "address_writer.hpp"
#include "preproccessed_block.hpp"
#include <blocksci/scripts/bitcoin_pubkey.hpp>

#include <secp256k1.h>

#include <iostream>


template<blocksci::AddressType::Enum type>
struct GenerateScriptInputFunctor {
    static ScriptInputType f(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &tx) {
        return ScriptInput<type>(inputView, scriptView, tx);
    }
};

template<>
struct GenerateScriptInputFunctor<blocksci::AddressType::Enum::MULTISIG> {
    static ScriptInputType f(const InputView &, const CScriptView &, const RawTransaction &) {
        assert(false);
    }
};

ScriptInputType generateScriptInput(const blocksci::Address &address, const InputView &inputView, const CScriptView &scriptView, const RawTransaction &tx) {
    
    static constexpr auto table = blocksci::make_dynamic_table<blocksci::AddressType, GenerateScriptInputFunctor>();
    static constexpr std::size_t size = blocksci::AddressType::all.size();
    
    auto index = static_cast<size_t>(address.type);
    if (index >= size)
    {
        throw std::invalid_argument("combination of enum values is not valid");
    }
    return table[index](inputView, scriptView, tx);
}

struct ScriptInputGenerator : public boost::static_visitor<ScriptInputType> {
    const InputView &inputView;
    const CScriptView &scriptView;
    const RawTransaction &tx;
    ScriptInputGenerator(const InputView &inputView_, const CScriptView &scriptView_, const RawTransaction &tx_) : inputView(inputView_), scriptView(scriptView_), tx(tx_) {}
    template <blocksci::AddressType::Enum type>
    ScriptInputType operator()(const ScriptOutput<type> &output) const {
        return ScriptInput<type>(inputView, scriptView, tx, output);
    }
};

ScriptInputType generateScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &tx, const ScriptOutputType &output) {
    ScriptInputGenerator visitor{inputView, scriptView, tx};
    return boost::apply_visitor(visitor, output);
}

struct ScriptInputProcessor : public boost::static_visitor<void> {
    AddressState &state;
    ScriptInputProcessor(AddressState &state_) : state(state_) {}
    template <blocksci::AddressType::Enum type>
    void operator()(ScriptInput<type> &input) const {
        input.processInput(state);
    }
};

void processScriptInput(ScriptInputType &input, AddressState &state) {
    ScriptInputProcessor visitor{state};
    return boost::apply_visitor(visitor, input);
}

struct ScriptInputSerializer : public boost::static_visitor<ProcessedInput> {
    uint32_t scriptNum;
    AddressWriter &addressWriter;
    ScriptInputSerializer(uint32_t scriptNum_, AddressWriter &addressWriter_) : scriptNum(scriptNum_), addressWriter(addressWriter_) {}
    template <blocksci::AddressType::Enum type>
    ProcessedInput operator()(ScriptInput<type> &input) const {
        return input.serializeInput(scriptNum, addressWriter);
    }
};

ProcessedInput serializeScriptInput(uint32_t scriptNum, ScriptInputType &input, AddressWriter &addressWriter) {
    ScriptInputSerializer visitor{scriptNum, addressWriter};
    return boost::apply_visitor(visitor, input);
}

struct ScriptInputChecker : public boost::static_visitor<void> {
    uint32_t scriptNum;
    const AddressState &state;
    const AddressWriter &addressWriter;
    ScriptInputChecker(uint32_t scriptNum_, const AddressState &state_, const AddressWriter &addressWriter_) : scriptNum(scriptNum_), state(state_), addressWriter(addressWriter_) {}
    template <blocksci::AddressType::Enum type>
    void operator()(ScriptInput<type> &input) const {
        input.checkInput(scriptNum, state, addressWriter);
    }
};

void checkScriptInput(uint32_t scriptNum, ScriptInputType &input, const AddressState &state, const AddressWriter &addressWriter) {
    ScriptInputChecker visitor{scriptNum, state, addressWriter};
    boost::apply_visitor(visitor, input);
}

ScriptInput<blocksci::AddressType::Enum::SCRIPTHASH>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &tx) : ScriptInputBase(inputView, scriptView) {
    
    CScriptView::const_iterator pc1 = scriptView.begin();
    CScriptView::const_iterator prevpc = scriptView.begin();
    CScriptView::const_iterator prevprevpc = scriptView.begin();
    opcodetype opcode1;
    boost::iterator_range<const unsigned char *> vch1;
    
    boost::iterator_range<const unsigned char *> lastScript;
    while(true) {
        prevprevpc = prevpc;
        prevpc = pc1;
        lastScript = vch1;
        if(!scriptView.GetOp(pc1, opcode1, vch1)) {
            break;
        }
    }
    
    CScriptView::const_iterator begin = scriptView.begin();
    wrappedInputBegin = &*begin;
    wrappedInputLength = static_cast<uint32_t>(std::distance(begin, prevprevpc));
    
    CScriptView wrappedOutputScript(lastScript.begin(), lastScript.end());
    wrappedScriptOutput = extractScriptData(lastScript.begin(), lastScript.end(), inputView.witnessActivated);
    
    CScriptView p2shScriptView{wrappedInputBegin, wrappedInputBegin + wrappedInputLength};
    wrappedScriptInput = std::make_unique<ScriptInputType>(generateScriptInput(inputView, p2shScriptView, tx, wrappedScriptOutput));
}

void ScriptInput<blocksci::AddressType::Enum::SCRIPTHASH>::processInput(AddressState &state) {
    std::tie(wrappedAddress, containsNewOutput) = resolveAddress(wrappedScriptOutput, state);
    processScriptInput(*wrappedScriptInput, state);
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::SCRIPTHASH>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    ProcessedInput processedInput = serializeScriptInput(wrappedAddress.scriptNum, *wrappedScriptInput, writer);
    bool firstSpend = writer.serialize(*this, scriptNum);
    if (firstSpend) {
        processedInput.push_back(scriptNum);
    }
    return processedInput;
}

void ScriptInput<blocksci::AddressType::Enum::SCRIPTHASH>::checkInput(uint32_t scriptNum, const AddressState &state, const AddressWriter &writer) {
    wrappedAddress = checkOutput(wrappedScriptOutput, state, writer);
    //
    CScriptView p2shScriptView{wrappedInputBegin, wrappedInputBegin + wrappedInputLength};
    checkScriptInput(wrappedAddress.scriptNum, *wrappedScriptInput, state, writer);
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::PUBKEY>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    writer.serialize(*this, scriptNum);
    return ProcessedInput{};
}

ScriptInput<blocksci::AddressType::Enum::PUBKEYHASH>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &) : ScriptInputBase(inputView, scriptView) {
    if (scriptView.size() > 0) {
        auto pc = scriptView.begin();
        opcodetype opcode;
        boost::iterator_range<const unsigned char *> vchSig;
        scriptView.GetOp(pc, opcode, vchSig);
        scriptView.GetOp(pc, opcode, vchSig);
        pubkey.Set(vchSig.begin(), vchSig.end());
    } else {
        auto &pubkeyWitness = inputView.witnessStack[1];
        pubkey.Set(reinterpret_cast<const unsigned char *>(pubkeyWitness.itemBegin), reinterpret_cast<const unsigned char *>(pubkeyWitness.itemBegin) + pubkeyWitness.length);
    }
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::PUBKEYHASH>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    writer.serialize(*this, scriptNum);
    return ProcessedInput{};
}

ScriptInput<blocksci::AddressType::Enum::MULTISIG>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &, const AddressWriter &writer) : ScriptInputBase(inputView, scriptView) {
    // Prelimary work on code to track multisig spend sets
    /*
    auto &multisigFile = writer.getFile<blocksci::AddressType::Enum::MULTISIG>();
    auto &pubkeyFile = writer.getFile<blocksci::AddressType::Enum::PUBKEY>();
    auto multisigData = multisigFile.getData(scriptNum - 1);
    auto addresses = multisigData->getAddresses();
    
    auto script = scriptView.getScript();
    
    CScriptView::const_iterator pc1 = script.begin();
    opcodetype opcode1;
    boost::iterator_range<const unsigned char *> vchSig;
    script.GetOp(pc1, opcode1, vchSig);
    
    size_t pubkeyNum = 0;
    
    while(true) {
        if(!script.GetOp(pc1, opcode1, vchSig)) {
            break;
        }
        if (vchSig.size() > 0) {
            int hashType = vchSig.back();
            vchSig.pop_back();
            auto txHash = tx.getHash(inputView, scriptView, hashType);
            
            while (pubkeyNum < addresses.size()) {
                auto pubkeyAddress = pubkeyFile.getData(addresses[pubkeyNum].addressNum - 1);
                if (pubkeyAddress->pubkey.Verify(txHash, vchSig)) {
                    spendSet[pubkeyNum] = 1;
                    pubkeyNum++;
                    break;
                } else {
                    spendSet[pubkeyNum] = 0;
                    pubkeyNum++;
                }
            }
        }
    }
    
    while (pubkeyNum < addresses.size()) {
        spendSet[pubkeyNum] = 0;
        pubkeyNum++;
    }
    */
}

ScriptInput<blocksci::AddressType::Enum::MULTISIG>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &tx, const ScriptOutput<blocksci::AddressType::Enum::MULTISIG> &spentOutput) : ScriptInputBase(inputView, scriptView) {
    // Prelimary work on code to track multisig spend sets
    
    CScriptView::const_iterator pc1 = scriptView.begin();
    opcodetype opcode1;
    boost::iterator_range<const unsigned char *> vchSig;
    scriptView.GetOp(pc1, opcode1, vchSig);
    
    size_t pubkeyNum = 0;
    
    while(true) {
        if(!scriptView.GetOp(pc1, opcode1, vchSig)) {
            break;
        }
        if (vchSig.size() > 0) {
            int hashType = vchSig.back();
            vchSig.pop_back();
            auto txHash = tx.getHash(inputView, scriptView, hashType);
            
            while (pubkeyNum < spentOutput.addressCount) {
                std::vector<unsigned char> sig{vchSig.begin(), vchSig.end()};
                if (spentOutput.addresses[pubkeyNum].Verify(txHash, sig)) {
                    spendSet[pubkeyNum] = 1;
                    pubkeyNum++;
                    break;
                } else {
                    spendSet[pubkeyNum] = 0;
                    pubkeyNum++;
                }
            }
        }
    }
    
    while (pubkeyNum < spentOutput.addressCount) {
        spendSet[pubkeyNum] = 0;
        pubkeyNum++;
    }
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::MULTISIG>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    writer.serialize(*this, scriptNum);
    return ProcessedInput{};
}

ScriptInput<blocksci::AddressType::Enum::NONSTANDARD>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &) : ScriptInputBase(inputView, scriptView) {
    if (scriptView.size() > 0) {
        script = CScript(scriptView.begin(), scriptView.end());
    } else if (inputView.witnessStack.size() > 0) {
        for (size_t i = 0; i < inputView.witnessStack.size() - 1; i++) {
            auto &stackItem = inputView.witnessStack[i];
            auto itemBegin = reinterpret_cast<const unsigned char *>(stackItem.itemBegin);
            script << std::vector<unsigned char>{itemBegin, itemBegin + stackItem.length};
        }
    }
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::NONSTANDARD>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    writer.serialize(*this, scriptNum);
    return ProcessedInput{};
}

ScriptInput<blocksci::AddressType::Enum::NULL_DATA>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &) : ScriptInputBase(inputView, scriptView) {
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::NULL_DATA>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    writer.serialize(*this, scriptNum);
    return ProcessedInput{};
}

ScriptInput<blocksci::AddressType::Enum::WITNESS_PUBKEYHASH>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &) : ScriptInputBase(inputView, scriptView) {
    auto &pubkeyWitness = inputView.witnessStack[1];
    pubkey.Set(reinterpret_cast<const unsigned char *>(pubkeyWitness.itemBegin), reinterpret_cast<const unsigned char *>(pubkeyWitness.itemBegin) + pubkeyWitness.length);
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::WITNESS_PUBKEYHASH>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {
    writer.serialize(*this, scriptNum);
    return ProcessedInput{};
}


ScriptInput<blocksci::AddressType::Enum::WITNESS_SCRIPTHASH>::ScriptInput(const InputView &inputView, const CScriptView &scriptView, const RawTransaction &tx) : ScriptInputBase(inputView, scriptView) {
    auto &witnessScriptItem = inputView.witnessStack.back();
    auto outputBegin = reinterpret_cast<const unsigned char *>(witnessScriptItem.itemBegin);
    wrappedScriptOutput = extractScriptData(outputBegin, outputBegin + witnessScriptItem.length, inputView.witnessActivated);
    wrappedScriptInput = std::make_unique<ScriptInputType>(generateScriptInput(inputView, scriptView, tx, wrappedScriptOutput));
}

void ScriptInput<blocksci::AddressType::Enum::WITNESS_SCRIPTHASH>::processInput(AddressState &state) {
    std::tie(wrappedAddress, containsNewOutput) = resolveAddress(wrappedScriptOutput, state);
    processScriptInput(*wrappedScriptInput, state);
}

ProcessedInput ScriptInput<blocksci::AddressType::Enum::WITNESS_SCRIPTHASH>::serializeInput(uint32_t scriptNum, AddressWriter &writer) {

    bool firstSpend = writer.serialize(*this, scriptNum);
    ProcessedInput processedInput = serializeScriptInput(wrappedAddress.scriptNum, *wrappedScriptInput, writer);
    if (firstSpend) {
        processedInput.push_back(scriptNum);
    }
    
    return processedInput;
}

void ScriptInput<blocksci::AddressType::Enum::WITNESS_SCRIPTHASH>::checkInput(uint32_t scriptNum, const AddressState &state, const AddressWriter &writer) {
    wrappedAddress = checkOutput(wrappedScriptOutput, state, writer);
    checkScriptInput(scriptNum, *wrappedScriptInput, state, writer);
}
