# Copyright (c) 2025 Dmitry Feklin (FeklinDN@gmail.com) Apache License 2.0.

import struct
import json
import regex as re
import unicodedata
import torch
from typing import List, Dict, Any, Tuple, Union, Literal
from dataclasses import dataclass, field
from transformers import AutoTokenizer

from TISA_tokenizer import TISACompiler, TISAVM, disassemble_TISA_manifest

def run_parity(model_id, text):
    print(f"\n--- TESTING: {model_id} ---")
    try:
        tok = AutoTokenizer.from_pretrained(model_id)

        manifest = TISACompiler.compile_and_calibrate(tok, text)
        state_for_res = json.loads(tok.backend_tokenizer.to_str())
        vm = TISAVM(TISACompiler._prepare_resources(tok, state_for_res))
        
        print("Disassembled Manifest:")
        disassembled_manifest = disassemble_TISA_manifest(manifest)
        print(json.dumps(disassembled_manifest, indent=2, ensure_ascii=False))
        
        vm_ids = vm.run(manifest, text)
        off_ids = tok.encode(text)
        
        print(f"\nPhrase: '{text}'")
        print(f"TISA IDs:    {vm_ids}")
        print(f"Ref IDs:     {off_ids}")
        
        if vm_ids != off_ids:
            print(f"MATCH:     False")
            print("--- TOKEN MISMATCH DETAILS ---")
            try:
                vm_tokens = [vm.id_to_token.get(i, f'[UNK_{i}]') for i in vm_ids]
                ref_tokens = tok.convert_ids_to_tokens(off_ids)
                print(f"TISA Tokens:  {vm_tokens}")
                print(f"Ref Tokens:   {ref_tokens}")
            except Exception as e:
                print(f"Could not get token details: {e}")
            print("----------------------------")
        else:
            print(f"MATCH:     True")
            
        vm_decode = vm.decode(vm_ids, skip_special_tokens=False)
        ref_decode = tok.decode(off_ids, skip_special_tokens=False)
        print(f"TISA Decode: '{vm_decode}'")
        print(f"Ref Decode:  '{ref_decode}'")

    except (AttributeError, TypeError, NotImplementedError) as e:
        print("\nNOTE: This model uses a Python-based ('slow') or non-standard tokenizer.")
        print(f"      The T-ISA compiler cannot process it automatically. Error: {type(e).__name__}")
        print("      Skipping VM compilation and parity check. Showing official output only.")
        
        try:
            if 'tok' not in locals(): 
                tok = AutoTokenizer.from_pretrained(model_id, use_fast=False)

            off_ids = tok.encode(text)
            ref_decode = tok.decode(off_ids, skip_special_tokens=False)
            
            print(f"\nPhrase:  '{text}'")
            print(f"Ref IDs:    {off_ids}")
            print(f"Ref Tokens: {tok.convert_ids_to_tokens(off_ids)}")
            print(f"Ref Decode:'{ref_decode}'")
        except Exception as load_err:
            print(f"Failed to process even with the official tokenizer. Error: {load_err}")

    except Exception as e:
        print(f"\nAn unexpected error occurred for {model_id}: {e}")

    finally:
        print("-" * (len(model_id) + 30))

run_parity("distilbert-base-uncased-finetuned-sst-2-english", "This movie is amazing!")
run_parity("gpt2", "The weather is nice today")
run_parity("t5-base", "The <extra_id_0> is nice today")
run_parity("FacebookAI/roberta-base", "The weather is nice today")
run_parity("xlm-roberta-base", "Hello world, мир! C'est la vie.")
run_parity("bert-base-cased", "OpenAI and Google are tech giants.")
run_parity("google/electra-small-discriminator", "A fast and efficient model.")
run_parity("dbmdz/bert-base-turkish-cased", "Bu Türkçe bir test metnidir.")
run_parity("EleutherAI/gpt-neo-125M", "GPT-Neo is a powerful model.")
run_parity("facebook/bart-large", "BART uses a BPE tokenizer.")
run_parity("bigscience/bloomz-560m", "Bloom is a multilingual model.")
run_parity("albert-base-v2", "ALBERT is a lighter BERT.")
run_parity("microsoft/deberta-v3-base", "DeBERTa represents a new generation.")
run_parity("bert-base-chinese", "北京是中国的首都")
run_parity("google/mt5-small", "This is a test sentence with numbers 123!")
run_parity("Qwen/Qwen2-1.5B", "Qwen2 is a multilingual model from Alibaba.")
run_parity("Salesforce/codet5-base", "public static void main(String[] args) {}")
run_parity("microsoft/phi-2", "def fibonacci(seq):")
run_parity("bert-base-german-cased", "Die schnelle braune Fuchs springt über den faulen Hund.")
run_parity("camembert-base", "Le chat est sur le tapis.")
run_parity("dccuchile/bert-base-spanish-wwm-cased", "El sol brilla en el cielo azul.")
run_parity("GroNLP/bert-base-dutch-cased", "De Grote Oceaan is de grootste oceaan ter wereld.")
run_parity("deepset/gbert-base", "Was ist die Hauptstadt von Deutschland?")
run_parity("Geotrend/bert-base-uk-cased", "Київ — столиця України.")
run_parity("aubmindlab/bert-base-arabertv2", "الذكاء الاصطناعي يغير العالم.")
run_parity("l3cube-pune/marathi-bert", "माझे नाव संगणक आहे.") # Маратхи
run_parity("sberbank-ai/rugpt3small_based_on_gpt2", "Пример текста для генеративной модели.")
run_parity("ai-forever/ruT5-base", "Это модель T5 для русского языка.")
run_parity("google-bert/bert-base-multilingual-cased", "Test sentence. Phrase de test. Тестовое предложение.")
run_parity("facebook/mbart-large-50", "This model supports 50 languages.")
run_parity("sentence-transformers/paraphrase-multilingual-mpnet-base-v2", "A sentence for multilingual embeddings.")
run_parity("distilbert/distilbert-base-multilingual-cased", "A distilled multilingual model.")
run_parity("bert-large-uncased", "A larger version of the BERT model.")
run_parity("roberta-large", "RoBERTa large model test.")
run_parity("google/flan-t5-large", "FLAN-T5 is an instruction-tuned model.")
run_parity("EleutherAI/gpt-neo-2.7B", "Testing a larger GPT-Neo model.")
run_parity("openai-community/gpt2-medium", "Medium-sized GPT-2 tokenizer.")
run_parity("sentence-transformers/all-MiniLM-L6-v2", "A popular model for sentence similarity.")
run_parity("allenai/longformer-base-4096", "This model can handle very long sequences of text.")
run_parity("emilyalsentzer/Bio_ClinicalBERT", "Patient shows symptoms of pneumonia.")
run_parity("BAAI/bge-large-en-v1.5", "Query: what is the best embedding model?")
run_parity("xlnet-base-cased", "XLNet is an autoregressive model.") # SentencePiece
run_parity("microsoft/mpnet-base", "MPNet uses a permuted language modeling objective.")
run_parity("squeezebert/squeezebert-uncased", "SqueezeBERT is a faster and smaller model.")
run_parity("YituTech/conv-bert-base", "ConvBERT is an efficient BERT variant.")
run_parity("stabilityai/stablelm-2-1_6b", "StableLM by Stability AI.")
run_parity("openai/whisper-large-v3", "<|startoftranscript|><|en|><|transcribe|><|notimestamps|>The quick brown fox jumps over the lazy dog.")
run_parity("allenai/scibert_scivocab_uncased", "The study of quantum chromodynamics requires advanced mathematics.")
run_parity("nlpaueb/legal-bert-base-uncased", "The plaintiff alleges that the defendant breached the contract.")
run_parity("ProsusAI/finbert", "The company's revenue increased by 15% year-over-year.")
run_parity("airesearch/wangchanberta-base-att-spm-uncased", "ภาษาไทยเป็นภาษาที่สวยงาม") 
run_parity("onlplab/alephbert-base", "שלום, זהו מבחן עבור טוקנייזר בעברית.")
run_parity("microsoft/layoutlm-base-uncased", "Testing document understanding models.")
run_parity("google/fnet-base", "FNet replaces self-attention with Fourier Transforms.")
run_parity("mistralai/Mistral-7B-v0.1", "Mistral is a new model.") 
run_parity("lmsys/vicuna-7b-v1.5", "Vicuna is a chat assistant fine-tuned from Llama.")
run_parity("sberbank-ai/rubert-base", "Тестируем русскоязычный токенизатор от Сбера.")
run_parity("codellama/CodeLlama-7b-hf", "def factorial(n): return 1 if n == 0 else n * factorial(n-1)")
run_parity("NousResearch/Llama-2-7b-chat-hf", "Llama-2 is a modern LLM.")
run_parity("TinyLlama/TinyLlama-1.1B-Chat-v1.0", "This is the TinyLlama model.")
run_parity("mistralai/Mixtral-8x7B-v0.1", "Mixtral uses a Mixture of Experts.")

run_parity("openai/clip-vit-base-patch32", "Requires revision or manual editing of the manifest")
run_parity("deepseek-ai/deepseek-coder-6.7b-instruct", "# This is a Python comment\nprint('Requires revision or manual editing of the manifest')")



