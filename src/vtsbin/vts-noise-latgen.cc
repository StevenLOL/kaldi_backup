// gmmbin/gmm-latgen-faster.cc

// Copyright 2009-2012  Microsoft Corporation
//                      Johns Hopkins University (author: Daniel Povey)
//                2014  Guoguo Chen

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.


#include "base/kaldi-common.h"
#include "util/common-utils.h"
#include "gmm/am-diag-gmm.h"
#include "tree/context-dep.h"
#include "hmm/transition-model.h"
#include "fstext/fstext-lib.h"
#include "decoder/lattice-faster-decoder.h"
#include "gmm/decodable-am-diag-gmm.h"
#include "util/timer.h"
#include "feat/feature-functions.h"  // feature reversal
#include "vts/vts-first-order.h"

int main(int argc, char *argv[]) {
  try {
    using namespace kaldi;
    typedef kaldi::int32 int32;
    using fst::SymbolTable;
    using fst::VectorFst;
    using fst::StdArc;

    const char *usage =
        "Generate lattices using VTS compensated GMM model.\n"
        "Usage: vts-noise-latgen [options] model-in (fst-in|fsts-rspecifier) features-rspecifier"
        " noiseparams-rspecifier lattice-wspecifier [ words-wspecifier [alignments-wspecifier] ]\n";
    ParseOptions po(usage);
    Timer timer;
    bool allow_partial = false;
    BaseFloat acoustic_scale = 0.1;
    int32 num_cepstral = 13;
    int32 num_fbank = 26;
    BaseFloat ceplifter = 22;
    LatticeFasterDecoderConfig config;
    
    std::string word_syms_filename;
    config.Register(&po);
    po.Register("num-cepstral", &num_cepstral, "Number of Cepstral features");
    po.Register("num-fbank", &num_fbank,
                "Number of FBanks used to generate the Cepstral features");
    po.Register("ceplifter", &ceplifter,
                "CepLifter value used for feature extraction");
    po.Register("acoustic-scale", &acoustic_scale,
                "Scaling factor for acoustic likelihoods");
    po.Register("word-symbol-table", &word_syms_filename,
                "Symbol table for words [for debug output]");
    po.Register("allow-partial", &allow_partial,
                "If true, produce output even if end state was not reached.");
    
    po.Read(argc, argv);

    if (po.NumArgs() < 5 || po.NumArgs() > 7) {
      po.PrintUsage();
      exit(1);
    }

    std::string model_in_filename = po.GetArg(1),
        fst_in_str = po.GetArg(2),
        feature_rspecifier = po.GetArg(3),
        noiseparams_rspecifier = po.GetArg(4),
        lattice_wspecifier = po.GetArg(5),
        words_wspecifier = po.GetOptArg(6),
        alignment_wspecifier = po.GetOptArg(7);
    
    TransitionModel trans_model;
    AmDiagGmm am_gmm;
    {
      bool binary;
      Input ki(model_in_filename, &binary);
      trans_model.Read(ki.Stream(), binary);
      am_gmm.Read(ki.Stream(), binary);
    }

    bool determinize = config.determinize_lattice;
    CompactLatticeWriter compact_lattice_writer;
    LatticeWriter lattice_writer;
    if (! (determinize ? compact_lattice_writer.Open(lattice_wspecifier)
           : lattice_writer.Open(lattice_wspecifier)))
      KALDI_ERR << "Could not open table for writing lattices: "
                 << lattice_wspecifier;

    Int32VectorWriter words_writer(words_wspecifier);

    Int32VectorWriter alignment_writer(alignment_wspecifier);

    fst::SymbolTable *word_syms = NULL;
    if (word_syms_filename != "") 
      if (!(word_syms = fst::SymbolTable::ReadText(word_syms_filename)))
        KALDI_ERR << "Could not read symbol table from file "
                   << word_syms_filename;

    Matrix<double> dct_mat, inv_dct_mat;
    GenerateDCTmatrix(num_cepstral, num_fbank, ceplifter, &dct_mat,
                      &inv_dct_mat);

    double tot_like = 0.0;
    kaldi::int64 frame_count = 0;
    int num_done = 0, num_err = 0;

    if (ClassifyRspecifier(fst_in_str, NULL, NULL) == kNoRspecifier) {
      SequentialBaseFloatMatrixReader feature_reader(feature_rspecifier);
      RandomAccessDoubleVectorReader noiseparams_reader(noiseparams_rspecifier);
      // Input FST is just one FST, not a table of FSTs.
      VectorFst<StdArc> *decode_fst = fst::ReadFstKaldi(fst_in_str);
      
      {
        LatticeFasterDecoder decoder(*decode_fst, config);
    
        for (; !feature_reader.Done(); feature_reader.Next()) {
          std::string utt = feature_reader.Key();
          Matrix<BaseFloat> features (feature_reader.Value());
          feature_reader.FreeCurrent();

          if (features.NumRows() == 0) {
            KALDI_WARN << "Zero-length utterance: " << utt;
            num_err++;
            continue;
          }
          
          KALDI_VLOG(1) << "Current utterance: " << utt;
          if (!noiseparams_reader.HasKey(utt + "_mu_h")
              || !noiseparams_reader.HasKey(utt + "_mu_z")
              || !noiseparams_reader.HasKey(utt + "_var_z")) {
            KALDI_ERR
                << "Not all the noise parameters (mu_h, mu_z, var_z) are available!";
          }

          int32 feat_dim = features.NumCols();
          if (feat_dim != num_cepstral * 3) {
            KALDI_ERR
                << "Could not decode the features, only " << num_cepstral *3
                << "D MFCC_0_D_A is supported!";
          }

          /************************************************
           Extract the noise parameters
           *************************************************/

          Vector<double> mu_h(noiseparams_reader.Value(utt + "_mu_h"));
          Vector<double> mu_z(noiseparams_reader.Value(utt + "_mu_z"));
          Vector<double> var_z(noiseparams_reader.Value(utt + "_var_z"));

          if (g_kaldi_verbose_level >= 1) {
            KALDI_LOG << "Additive Noise Mean: " << mu_z;
            KALDI_LOG << "Additive Noise Covariance: " << var_z;
            KALDI_LOG << "Convoluational Noise Mean: " << mu_h;
          }

          /************************************************
           Compensate the model
           *************************************************/

          AmDiagGmm noise_am_gmm;
          // Initialize with the clean speech model
          noise_am_gmm.CopyFromAmDiagGmm(am_gmm);

          std::vector<Matrix<double> > Jx(am_gmm.NumGauss()), Jz(am_gmm.NumGauss());  // not necessary for compensation only
          CompensateModel(mu_h, mu_z, var_z, num_cepstral, num_fbank, dct_mat,
                          inv_dct_mat, noise_am_gmm, Jx, Jz);


          DecodableAmDiagGmmScaled gmm_decodable(noise_am_gmm, trans_model, features,
                                                 acoustic_scale);

          double like;
          if (DecodeUtteranceLatticeFaster(
                  decoder, gmm_decodable, trans_model, word_syms, utt,
                  acoustic_scale, determinize, allow_partial, &alignment_writer,
                  &words_writer, &compact_lattice_writer, &lattice_writer,
                  &like)) {
            tot_like += like;
            frame_count += features.NumRows();
            num_done++;
          } else num_err++;
        }
      }
      delete decode_fst; // delete this only after decoder goes out of scope.
    } else { // We have different FSTs for different utterances.
      SequentialTableReader<fst::VectorFstHolder> fst_reader(fst_in_str);
      RandomAccessBaseFloatMatrixReader feature_reader(feature_rspecifier);          
      RandomAccessDoubleVectorReader noiseparams_reader(noiseparams_rspecifier);
      for (; !fst_reader.Done(); fst_reader.Next()) {
        std::string utt = fst_reader.Key();
        if (!feature_reader.HasKey(utt)) {
          KALDI_WARN << "Not decoding utterance " << utt
                     << " because no features available.";
          num_err++;
          continue;
        }
        const Matrix<BaseFloat> &features = feature_reader.Value(utt);
        if (features.NumRows() == 0) {
          KALDI_WARN << "Zero-length utterance: " << utt;
          num_err++;
          continue;
        }

        KALDI_VLOG(1) << "Current utterance: " << utt;
        if (!noiseparams_reader.HasKey(utt + "_mu_h")
            || !noiseparams_reader.HasKey(utt + "_mu_z")
            || !noiseparams_reader.HasKey(utt + "_var_z")) {
          KALDI_ERR
              << "Not all the noise parameters (mu_h, mu_z, var_z) are available!";
        }

        int32 feat_dim = features.NumCols();
        if (feat_dim != num_cepstral * 3) {
          KALDI_ERR
              << "Could not decode the features, only " << num_cepstral *3
              << "D MFCC_0_D_A is supported!";
        }

        /************************************************
         Extract the noise parameters
         *************************************************/

        Vector<double> mu_h(noiseparams_reader.Value(utt + "_mu_h"));
        Vector<double> mu_z(noiseparams_reader.Value(utt + "_mu_z"));
        Vector<double> var_z(noiseparams_reader.Value(utt + "_var_z"));

        if (g_kaldi_verbose_level >= 1) {
          KALDI_LOG << "Additive Noise Mean: " << mu_z;
          KALDI_LOG << "Additive Noise Covariance: " << var_z;
          KALDI_LOG << "Convoluational Noise Mean: " << mu_h;
        }

        /************************************************
         Compensate the model
         *************************************************/

        AmDiagGmm noise_am_gmm;
        // Initialize with the clean speech model
        noise_am_gmm.CopyFromAmDiagGmm(am_gmm);

        std::vector<Matrix<double> > Jx(am_gmm.NumGauss()), Jz(am_gmm.NumGauss());  // not necessary for compensation only
        CompensateModel(mu_h, mu_z, var_z, num_cepstral, num_fbank, dct_mat,
                        inv_dct_mat, noise_am_gmm, Jx, Jz);


        LatticeFasterDecoder decoder(fst_reader.Value(), config);
        DecodableAmDiagGmmScaled gmm_decodable(noise_am_gmm, trans_model, features,
                                               acoustic_scale);
        double like;
        if (DecodeUtteranceLatticeFaster(
                decoder, gmm_decodable, trans_model, word_syms, utt,
                acoustic_scale, determinize, allow_partial, &alignment_writer,
                &words_writer, &compact_lattice_writer, &lattice_writer,
                &like)) {
          tot_like += like;
          frame_count += features.NumRows();
          num_done++;
        } else num_err++;
      }
    }
      
    double elapsed = timer.Elapsed();
    KALDI_LOG << "Time taken "<< elapsed
              << "s: real-time factor assuming 100 frames/sec is "
              << (elapsed*100.0/frame_count);
    KALDI_LOG << "Done " << num_done << " utterances, failed for "
              << num_err;
    KALDI_LOG << "Overall log-likelihood per frame is " << (tot_like/frame_count) << " over "
              << frame_count << " frames.";

    if (word_syms) delete word_syms;
    if (num_done != 0) return 0;
    else return 1;
  } catch(const std::exception &e) {
    std::cerr << e.what();
    return -1;
  }
}
