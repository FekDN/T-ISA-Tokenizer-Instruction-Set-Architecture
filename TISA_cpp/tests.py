# Copyright (c) 2025 Dmitry Feklin (FeklinDN@gmail.com) Apache License 2.0.
# Генератор тестового набора для TISA VM на C++

import struct
import json
import os
import hashlib
from transformers import AutoTokenizer
from TISA_tokenizer import TISACompiler

def save_model_resources(tokenizer, model_state, output_path):
    os.makedirs(output_path, exist_ok=True)
    model_type = model_state.get('model', {}).get('type')
    print(f"  Model type: {model_type}")

    # vocab.b + vocab_idx.b (оставляем как было)
    vocab = tokenizer.get_vocab()
    sorted_vocab = sorted(vocab.items(), key=lambda item: item[0])
    scores = {}
    if model_type == 'Unigram':
        scores = {item[0]: item[1] for item in model_state['model']['vocab']}

    with open(os.path.join(output_path, "vocab.b"), "wb") as f:
        f.write(struct.pack("<I", len(sorted_vocab)))
        data_blobs = bytearray()
        offsets = []
        for token, token_id in sorted_vocab:
            offsets.append(len(data_blobs))
            token_bytes = token.encode('utf-8')
            score = scores.get(token, 0.0)
            data_blobs.extend(struct.pack("<H", len(token_bytes)))
            data_blobs.extend(token_bytes)
            data_blobs.extend(struct.pack("<i", token_id))
            data_blobs.extend(struct.pack("<f", score))
        for offset in offsets:
            f.write(struct.pack("<I", offset))
        f.write(data_blobs)

    # vocab_idx.b (оставляем как было)
    id_to_data_offset = {token_id: offsets[i] for i, (token, token_id) in enumerate(sorted_vocab)}
    max_id = max(id_to_data_offset.keys()) if id_to_data_offset else 0
    direct_lookup_table = [0xFFFFFFFF] * (max_id + 1)
    for token_id, data_offset in id_to_data_offset.items():
        direct_lookup_table[token_id] = data_offset
    with open(os.path.join(output_path, "vocab_idx.b"), "wb") as f:
        f.write(struct.pack("<I", len(direct_lookup_table)))
        for offset_val in direct_lookup_table:
            f.write(struct.pack("<I", offset_val))

    merges = model_state.get('model', {}).get('merges', [])
    if not merges and model_type == 'BPE':
        # fallback для gpt2, roberta, bart и т.д.
        if hasattr(tokenizer.backend_tokenizer.model, 'merges'):
            merges = tokenizer.backend_tokenizer.model.merges
            print(f"  Fallback merges loaded: {len(merges)} rules")

    if merges:
        merge_list = []
        for rank, merge_rule in enumerate(merges):
            if isinstance(merge_rule, str):
                parts = merge_rule.split(' ', 1)
                if len(parts) == 2:
                    merge_list.append((parts[0], parts[1], rank))
            elif isinstance(merge_rule, (list, tuple)):
                merge_list.append((merge_rule[0], merge_rule[1], rank))

        merge_list.sort(key=lambda x: (x[0], x[1]))

        with open(os.path.join(output_path, "merges.b"), "wb") as f:
            f.write(struct.pack("<I", len(merge_list)))
            data_blobs = bytearray()
            offsets = []
            for p1, p2, rank in merge_list:
                offsets.append(len(data_blobs))
                p1_bytes = p1.encode('utf-8')
                p2_bytes = p2.encode('utf-8')
                data_blobs.extend(struct.pack("<H", len(p1_bytes)))
                data_blobs.extend(p1_bytes)
                data_blobs.extend(struct.pack("<H", len(p2_bytes)))
                data_blobs.extend(p2_bytes)
                data_blobs.extend(struct.pack("<i", rank))
            for offset in offsets:
                f.write(struct.pack("<I", offset))
            f.write(data_blobs)

        print(f"  -> merges.b saved: {len(merge_list)} rules")
    else:
        print("  -> no merges (WordPiece/Unigram model)")

def generate_test_suite(test_cases, base_output_dir="tisa_build"):
    suite_filename = os.path.join(base_output_dir, "tisa_test_suite.bin")
    models_dir = os.path.join(base_output_dir, "models")
    os.makedirs(models_dir, exist_ok=True)
    
    valid_test_cases = []
    processed_models = set()
    model_name_map = {}
    
    for model_id, text in test_cases:
        print(f"Processing: {model_id}...")
        try:
            tok = AutoTokenizer.from_pretrained(model_id)
            manifest = TISACompiler.compile_and_calibrate(tok, text)
            off_ids = tok.encode(text)
            
            valid_test_cases.append({
                "model_id": model_id,
                "text": text,
                "manifest": manifest,
                "ref_ids": off_ids
            })

            if model_id not in processed_models:
                model_hash = hashlib.sha256(model_id.encode('utf-8')).hexdigest()[:8]
                model_output_path = os.path.join(models_dir, model_hash)
                model_name_map[model_id] = model_hash
                
                model_state = json.loads(tok.backend_tokenizer.to_str())
                save_model_resources(tok, model_state, model_output_path)
                processed_models.add(model_id)
        except Exception as e:
            print(f"  ✗ Skipping due to error: {e}")

    with open(os.path.join(models_dir, "model_map.txt"), "w", encoding='utf-8') as f:
        for full_name, hash_name in sorted(model_name_map.items()):
            f.write(f"{full_name}:{hash_name}\n")

    with open(suite_filename, "wb") as f:
        f.write(b"TSTS")
        f.write(struct.pack("<I", len(valid_test_cases)))
        for case in valid_test_cases:
            m_id = case["model_id"].encode('utf-8')
            txt = case["text"].encode('utf-8')
            f.write(struct.pack("<H", len(m_id)))
            f.write(m_id)
            f.write(struct.pack("<I", len(txt)))
            f.write(txt)
            f.write(struct.pack("<I", len(case["manifest"])))
            f.write(case["manifest"])
            f.write(struct.pack("<I", len(case["ref_ids"])))
            for tid in case["ref_ids"]: f.write(struct.pack("<i", tid))

if __name__ == "__main__":
    test_cases = [
        ("distilbert-base-uncased-finetuned-sst-2-english", "This movie is amazing"),
        ("gpt2", "The weather is nice today"),
        ("t5-base", "The <extra_id_0> is nice today"),
        ("FacebookAI/roberta-base", "The weather is nice today"),
        ("xlm-roberta-base", "Hello world, мир! C'est la vie."),
        ("bert-base-cased", "OpenAI and Google are tech giants."),
        ("google/electra-small-discriminator", "A fast and efficient model."),
        ("dbmdz/bert-base-turkish-cased", "Bu Türkçe bir test metnidir."),
        ("EleutherAI/gpt-neo-125M", "GPT-Neo is a powerful model."),
        ("facebook/bart-large", "BART uses a BPE tokenizer."),
        ("bigscience/bloomz-560m", "Bloom is a multilingual model."),
        ("albert-base-v2", "ALBERT is a lighter BERT."),
        ("microsoft/deberta-v3-base", "DeBERTa represents a new generation."),
        ("bert-base-chinese", "北京是中国的首都"),
        ("google/mt5-small", "This is a test sentence with numbers 123!"),
        ("Qwen/Qwen2-1.5B", "Qwen2 is a multilingual model from Alibaba."),
        ("Salesforce/codet5-base", "public static void main(String[] args), {}"),
        ("microsoft/phi-2", "def fibonacci(seq),:"),
        ("bert-base-german-cased", "Die schnelle braune Fuchs springt über den faulen Hund."),
        ("camembert-base", "Le chat est sur le tapis."),
        ("dccuchile/bert-base-spanish-wwm-cased", "El sol brilla en el cielo azul."),
        ("GroNLP/bert-base-dutch-cased", "De Grote Oceaan is de grootste oceaan ter wereld."),
        ("deepset/gbert-base", "Was ist die Hauptstadt von Deutschland?"),
        ("Geotrend/bert-base-uk-cased", "Київ — столиця України."),
        ("aubmindlab/bert-base-arabertv2", "الذكاء الاصطناعي يغير العالم."),
        ("l3cube-pune/marathi-bert", "माझे नाव संगणक आहे."), # Маратхи
        ("sberbank-ai/rugpt3small_based_on_gpt2", "Пример текста для генеративной модели."),
        ("ai-forever/ruT5-base", "Это модель T5 для русского языка."),
        ("google-bert/bert-base-multilingual-cased", "Test sentence. Phrase de test. Тестовое предложение."),
        ("facebook/mbart-large-50", "This model supports 50 languages."),
        ("sentence-transformers/paraphrase-multilingual-mpnet-base-v2", "A sentence for multilingual embeddings."),
        ("distilbert/distilbert-base-multilingual-cased", "A distilled multilingual model."),
        ("bert-large-uncased", "A larger version of the BERT model."),
        ("roberta-large", "RoBERTa large model test."),
        ("google/flan-t5-large", "FLAN-T5 is an instruction-tuned model."),
        ("EleutherAI/gpt-neo-2.7B", "Testing a larger GPT-Neo model."),
        ("openai-community/gpt2-medium", "Medium-sized GPT-2 tokenizer."),
        ("sentence-transformers/all-MiniLM-L6-v2", "A popular model for sentence similarity."),
        ("allenai/longformer-base-4096", "This model can handle very long sequences of text."),
        ("emilyalsentzer/Bio_ClinicalBERT", "Patient shows symptoms of pneumonia."),
        ("BAAI/bge-large-en-v1.5", "Query: what is the best embedding model?"),
        ("xlnet-base-cased", "XLNet is an autoregressive model."), # SentencePiece
        ("microsoft/mpnet-base", "MPNet uses a permuted language modeling objective."),
        ("squeezebert/squeezebert-uncased", "SqueezeBERT is a faster and smaller model."),
        ("YituTech/conv-bert-base", "ConvBERT is an efficient BERT variant."),
        ("stabilityai/stablelm-2-1_6b", "StableLM by Stability AI."),
        ("openai/whisper-large-v3", "<|startoftranscript|><|en|><|transcribe|><|notimestamps|>The quick brown fox jumps over the lazy dog."),
        ("allenai/scibert_scivocab_uncased", "The study of quantum chromodynamics requires advanced mathematics."),
        ("nlpaueb/legal-bert-base-uncased", "The plaintiff alleges that the defendant breached the contract."),
        ("ProsusAI/finbert", "The company's revenue increased by 15% year-over-year."),
        ("airesearch/wangchanberta-base-att-spm-uncased", "ภาษาไทยเป็นภาษาที่สวยงาม"), 
        ("onlplab/alephbert-base", "שלום, זהו מבחן עבור טוקנייזר בעברית."),
        ("microsoft/layoutlm-base-uncased", "Testing document understanding models."),
        ("google/fnet-base", "FNet replaces self-attention with Fourier Transforms."),
        ("mistralai/Mistral-7B-v0.1", "Mistral is a new model."), 
        ("lmsys/vicuna-7b-v1.5", "Vicuna is a chat assistant fine-tuned from Llama."),
        ("sberbank-ai/rubert-base", "Тестируем русскоязычный токенизатор от Сбера."),
        ("codellama/CodeLlama-7b-hf", "def factorial(n): return 1 if n == 0 else n * factorial(n-1)"),
        ("NousResearch/Llama-2-7b-chat-hf", "Llama-2 is a modern LLM."),
        ("TinyLlama/TinyLlama-1.1B-Chat-v1.0", "This is the TinyLlama model."),
        ("mistralai/Mixtral-8x7B-v0.1", "Mixtral uses a Mixture of Experts.")
    ]
    generate_test_suite(test_cases)