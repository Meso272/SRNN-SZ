#ifndef SZ3_SZINTERP_HPP
#define SZ3_SZINTERP_HPP

#include "QoZ/compressor/SZInterpolationCompressor.hpp"

#include "QoZ/compressor/deprecated/SZBlockInterpolationCompressor.hpp"

#include "QoZ/preprocessor/Wavelet.hpp"

#include "QoZ/quantizer/IntegerQuantizer.hpp"
#include "QoZ/lossless/Lossless_zstd.hpp"
#include "QoZ/utils/Iterator.hpp"
#include "QoZ/utils/Sample.hpp"
#include "QoZ/utils/Transform.hpp"
#include "QoZ/utils/Statistic.hpp"
#include "QoZ/utils/Extraction.hpp"
#include "QoZ/utils/QuantOptimization.hpp"
#include "QoZ/utils/Config.hpp"
#include "QoZ/utils/Metrics.hpp"
#include "QoZ/utils/CoeffRegression.hpp"
#include "QoZ/utils/ExtractRegData.hpp"
#include "QoZ/api/impl/SZLorenzoReg.hpp"

#include "QoZ/sperr/SPERR3D_OMP_C.h"

#include "QoZ/sperr/SPERR3D_OMP_D.h"


//#include <cunistd>
#include <cmath>
#include <memory>
#include <limits>
#include <cstring>
#include <cstdlib>

template<class T, QoZ::uint N>
bool use_sperr(const QoZ::Config & conf){
    return ( (conf.wavelet>0 or conf.sperrWithoutWave) and conf.sperr>=conf.wavelet and N==3);
}

template<class T, QoZ::uint N>
auto pre_Condition(const QoZ::Config &conf,T * data){
    std::cout<<"pre"<<std::endl;
    std::vector<double> buf(conf.num,0);
    for(size_t i=0;i<conf.num;i++)
        buf[i]=data[i];
    std::cout<<"pre2"<<std::endl;
    sperr::Conditioner conditioner;
    if(conf.conditioning==2){
        std::array<bool, 4> b4{true,true,false,false};
        conditioner.toggle_all_settings(b4);
    }
    auto [rtn, condi_meta] = conditioner.condition(buf);
    if(rtn!=sperr::RTNType::Good)
        std::cout<<"bad cond"<<std::endl;
    std::cout<<"pre3"<<std::endl;
    for(size_t i=0;i<conf.num;i++)
        data[i]=buf[i];
    std::cout<<"pre4"<<std::endl;
    return condi_meta;
}

template<class T, QoZ::uint N>
auto post_Condition(T * data,const size_t &num,const sperr::Conditioner::meta_type& meta){
    //std::cout<<"post"<<std::endl;
    std::vector<double> buf(num,0);
    for(size_t i=0;i<num;i++)
        buf[i]=data[i];

    sperr::Conditioner conditioner;
    auto rtn = conditioner.inverse_condition(buf,meta);
    for(size_t i=0;i<num;i++)
        data[i]=buf[i];
    return rtn;
}

template<class T, QoZ::uint N> 
char *SPERR_Compress(QoZ::Config &conf, T *data, size_t &outSize){
        
    SPERR3D_OMP_C compressor;
    compressor.set_num_threads(1);
    //std::cout<<conf.wavelet_rel_coeff<<std::endl;
    compressor.set_eb_coeff(conf.wavelet_rel_coeff);
    if(conf.wavelet!=1)
        compressor.set_skip_wave(true);
    //std::cout<<"s1"<<std::endl;
    auto rtn = sperr::RTNType::Good;
      
    auto chunks = std::vector<size_t>{1024,1024, 1024};//ori 256^3
    rtn = compressor.copy_data(reinterpret_cast<const float*>(data), conf.num,
                                {conf.dims[2], conf.dims[1], conf.dims[0]}, {chunks[0], chunks[1], chunks[2]});
    //std::cout<<"s2"<<std::endl;
    //std::cout<<conf.absErrorBound<<std::endl;
    compressor.set_target_pwe(conf.absErrorBound);


    
    //std::cout<<"s3"<<std::endl;
    rtn = compressor.compress();
    //std::cout<<"s4"<<std::endl;
    auto stream = compressor.get_encoded_bitstream();
        
    char * outData=new char[stream.size()+conf.size_est()];
    outSize=stream.size();
    //std::cout<<outSize<<std::endl;
    memcpy(outData,stream.data(),stream.size());
    stream.clear();
    stream.shrink_to_fit();

    //std::cout<<"s5"<<std::endl;
    return outData;

}
template<class T, QoZ::uint N> 
void SPERR_Decompress(char *cmpData, size_t cmpSize, T *decData){
    
    std::vector<uint8_t> in_stream(cmpData,cmpData+cmpSize);
    SPERR3D_OMP_D decompressor;
  
    decompressor.set_num_threads(1);
    if (decompressor.use_bitstream(in_stream.data(), in_stream.size()) != sperr::RTNType::Good) {
        std::cerr << "Read compressed file error: "<< std::endl;
        return;
    }

    if (decompressor.decompress(in_stream.data()) != sperr::RTNType::Good) {
        std::cerr << "Decompression failed!" << std::endl;
        return ;
    }
   
    in_stream.clear();
    in_stream.shrink_to_fit();
    const auto vol = decompressor.get_data<float>();
    //std::cout<<vol.size()<<std::endl;
    memcpy(decData,vol.data(),sizeof(T)*vol.size());
    
    //decData=vol.data();
    return;
}


template<class T, QoZ::uint N>
char * outlier_compress(QoZ::Config &conf,T *data,size_t &outSize){

    char * outlier_compress_output;
    if (conf.offsetPredictor ==0){
        auto quantizer = QoZ::LinearQuantizer<T>(conf.absErrorBound, conf.quantbinCnt / 2);
        auto sz = QoZ::make_sz_general_compressor<T, 1>(QoZ::make_sz_general_frontend<T, 1>(conf, QoZ::ZeroPredictor<T, 1>(), quantizer), QoZ::HuffmanEncoder<int>(),
                                                                       QoZ::Lossless_zstd());  
        outlier_compress_output =  (char *)sz->compress(conf,data,outSize);
        delete sz;
        //std::cout<<"p6"<<std::endl;
        //std::cout<<outlier_outSize<<std::endl;
    }
    else if (conf.offsetPredictor ==1){
        conf.lorenzo = true;
        conf.lorenzo2 = true;
        conf.regression = false;
        conf.regression2 = false;
        conf.openmp = false;
        conf.blockSize = 16;//original 5
        conf.quantbinCnt = 65536 * 2;

        auto quantizer = QoZ::LinearQuantizer<T>(conf.absErrorBound, conf.quantbinCnt / 2);
        auto sz = make_lorenzo_regression_compressor<T, 1>(conf, quantizer, QoZ::HuffmanEncoder<int>(), QoZ::Lossless_zstd());
        outlier_compress_output =  (char *)sz->compress(conf,data,outSize);
        delete sz;
    }
    else if (conf.offsetPredictor == 2){
        conf.setDims(conf.dims.begin(),conf.dims.end());
        conf.lorenzo = true;
        conf.lorenzo2 = true;
        conf.regression = false;
        conf.regression2 = false;
        conf.openmp = false;
        conf.blockSize = 5;
        conf.quantbinCnt = 65536 * 2;
        auto quantizer = QoZ::LinearQuantizer<T>(conf.absErrorBound, conf.quantbinCnt / 2);
        auto sz = make_lorenzo_regression_compressor<T, N>(conf, quantizer, QoZ::HuffmanEncoder<int>(), QoZ::Lossless_zstd());
        outlier_compress_output =  (char *)sz->compress(conf,data,outSize);
        delete sz;
    }

    else if (conf.offsetPredictor == 3){
        conf.interpAlgo=QoZ::INTERP_ALGO_CUBIC;
        conf.interpDirection=0;
        auto sz = QoZ::SZInterpolationCompressor<T, 1, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
        QoZ::LinearQuantizer<T>(conf.absErrorBound),
        QoZ::HuffmanEncoder<int>(),
        QoZ::Lossless_zstd());
        outlier_compress_output =  (char *)sz.compress(conf,data,outSize);
        
    }

    else if (conf.offsetPredictor == 4){
            
        conf.setDims(conf.dims.begin(),conf.dims.end());
        conf.interpAlgo=QoZ::INTERP_ALGO_CUBIC;
        conf.interpDirection=0;
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
        QoZ::LinearQuantizer<T>(conf.absErrorBound),
        QoZ::HuffmanEncoder<int>(),
        QoZ::Lossless_zstd());
            
       
        outlier_compress_output =  (char *)sz.compress(conf,data,outSize);
        
    }
    return outlier_compress_output;

}

template<class T, QoZ::uint N>
void outlier_decompress(QoZ::Config &conf,char *cmprData,size_t outSize,T*decData){
    if (conf.offsetPredictor ==0){
        auto quantizer = QoZ::LinearQuantizer<T>(conf.absErrorBound,conf.quantbinCnt / 2);
        auto sz = QoZ::make_sz_general_compressor<T, 1>(QoZ::make_sz_general_frontend<T, 1>(conf, QoZ::ZeroPredictor<T, 1>(), quantizer), QoZ::HuffmanEncoder<int>(),
                                                                       QoZ::Lossless_zstd());

        //std::cout<<"dwa"<<std::endl;
        sz->decompress((QoZ::uchar *)cmprData,outSize,decData);
        //std::cout<<"dwa"<<std::endl;
        delete sz;
    }

    else if (conf.offsetPredictor ==1){
        conf.lorenzo = true;
        conf.lorenzo2 = true;
        conf.regression = false;
        conf.regression2 = false;
        conf.openmp = false;
        conf.blockSize = 16;//original 5
        conf.quantbinCnt = 65536 * 2;

        auto quantizer = QoZ::LinearQuantizer<T>(conf.absErrorBound, conf.quantbinCnt / 2);
        auto sz = make_lorenzo_regression_compressor<T, 1>(conf, quantizer, QoZ::HuffmanEncoder<int>(), QoZ::Lossless_zstd());
                  
        sz->decompress((QoZ::uchar *)cmprData,outSize,decData);
        delete sz;
    }
    else if (conf.offsetPredictor == 2){
        conf.setDims(conf.dims.begin(),conf.dims.end());
        conf.lorenzo = true;
        conf.lorenzo2 = true;
        conf.regression = false;
        conf.regression2 = false;
        conf.openmp = false;
        conf.blockSize = 5;
        conf.quantbinCnt = 65536 * 2;

        auto quantizer = QoZ::LinearQuantizer<T>(conf.absErrorBound, conf.quantbinCnt / 2);
        auto sz = make_lorenzo_regression_compressor<T, N>(conf, quantizer, QoZ::HuffmanEncoder<int>(), QoZ::Lossless_zstd());       
        sz->decompress((QoZ::uchar *)cmprData,outSize,decData);
        delete sz;
    }

    else if (conf.offsetPredictor == 3){
        conf.interpAlgo=QoZ::INTERP_ALGO_CUBIC;
        conf.interpDirection=0;

           
        auto sz = QoZ::SZInterpolationCompressor<T, 1, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
        QoZ::LinearQuantizer<T>(conf.absErrorBound),
        QoZ::HuffmanEncoder<int>(),
        QoZ::Lossless_zstd());      
        sz.decompress((QoZ::uchar *)cmprData,outSize,decData);
        
    }

    else if (conf.offsetPredictor == 4){
            
        conf.setDims(conf.dims.begin(),conf.dims.end());
        conf.interpAlgo=QoZ::INTERP_ALGO_CUBIC;
        conf.interpDirection=0;          
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
        QoZ::LinearQuantizer<T>(conf.absErrorBound),
        QoZ::HuffmanEncoder<int>(),
        QoZ::Lossless_zstd());      
        sz.decompress((QoZ::uchar *)cmprData,outSize,decData);
        
    }
    
}

template<class T, QoZ::uint N>
char *SZ_compress_Interp(QoZ::Config &conf, T *data, size_t &outSize) {

//    std::cout << "****************** Interp Compression ****************" << std::endl;
//    std::cout << "Interp Op          = " << interpAlgo << std::endl
//              << "Direction          = " << direction << std::endl
//              << "SZ block size      = " << blockSize << std::endl
//              << "Interp block size  = " << interpBlockSize << std::endl;

    assert(N == conf.N);
    assert(conf.cmprAlgo == QoZ::ALGO_INTERP);
    QoZ::calAbsErrorBound(conf, data);

    //conf.print();
    
    auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
            QoZ::LinearQuantizer<T>(conf.absErrorBound),
            QoZ::HuffmanEncoder<int>(),
            QoZ::Lossless_zstd());

    
   
    //QoZ::Timer timer;

    //timer.start();
    char *cmpData = (char *) sz.compress(conf, data, outSize);
     //double incall_time = timer.stop();
    //std::cout << "incall time = " << incall_time << "s" << std::endl;
    return cmpData;
}

template<class T, QoZ::uint N>
void SZ_decompress_Interp(const QoZ::Config &conf, char *cmpData, size_t cmpSize, T *decData) {
    assert(conf.cmprAlgo == QoZ::ALGO_INTERP);
    QoZ::uchar const *cmpDataPos = (QoZ::uchar *) cmpData;
    //std::cout<<"x0"<<std::endl;
    if (conf.wavelet==0 and !use_sperr<T,N>(conf)){
        
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                QoZ::LinearQuantizer<T>(),
                QoZ::HuffmanEncoder<int>(),
                QoZ::Lossless_zstd());
        if (!conf.blockwiseTuning)
            sz.decompress(cmpDataPos, cmpSize, decData);
        else{
            //std::cout<<"block decomp"<<std::endl;
            sz.decompress_block(cmpDataPos, cmpSize, decData);
        }
    }
    
    else{


        if(use_sperr<T,N>(conf) and conf.wavelet<=1){
            //std::cout<<cmpSize<<std::endl;
            std::vector<uint8_t> in_stream(cmpData,cmpData+cmpSize);
            SPERR3D_OMP_D decompressor;
            //std::cout<<"d1"<<std::endl;
            decompressor.set_num_threads(1);
            if (decompressor.use_bitstream(in_stream.data(), in_stream.size()) != sperr::RTNType::Good) {
                std::cerr << "Read compressed file error: "<< std::endl;
                return;
            }

            if (decompressor.decompress(in_stream.data()) != sperr::RTNType::Good) {
                std::cerr << "Decompression failed!" << std::endl;
                return ;
            }
            in_stream.clear();
            in_stream.shrink_to_fit();
            const auto vol = decompressor.get_data<float>();
            memcpy(decData,vol.data(),sizeof(T)*conf.num);
            //std::cout<<"d2"<<std::endl;
            //decData=vol.data();
            return;




        }
        //std::cout<<"x1"<<std::endl;
        size_t first =conf.firstSize;
        size_t second=cmpSize-conf.firstSize;    
        //QoZ::uchar const *cmpDataFirst = new QoZ::uchar [first];
        //QoZ::uchar const *cmpDataSecond = new QoZ::uchar [second];
       // memcpy(cmpDataFirst,cmpData,first);
        //memcpy(cmpDataSecond,cmpData+first,second);

        if(use_sperr<T,N>(conf))
            SPERR_Decompress<T,N>((char*)cmpDataPos, first,decData);
        else{
            auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                    QoZ::LinearQuantizer<T>(),
                    QoZ::HuffmanEncoder<int>(),
                    QoZ::Lossless_zstd());

        
            if (!conf.blockwiseTuning)
                sz.decompress(cmpDataPos, first, decData);
            else{
                //std::cout<<"block decomp"<<std::endl;
                sz.decompress_block(cmpDataPos, first, decData);
            }
        }
      
        //QoZ::writefile<T>("waved.qoz.dec.sigmo", decData, conf.num);
        /*
        if(conf.transformation==1){
            for(size_t i=0;i<conf.num;i++)
                decData[i]=QoZ::logit<double>(decData[i]);
        }
        else if(conf.transformation==2){
            for(size_t i=0;i<conf.num;i++)
                decData[i]=QoZ::arctanh<double>(decData[i]);
        } 
        */

         //QoZ::writefile<T>("waved.qoz.dec.logit", decData, conf.num);
        if(conf.wavelet>1){


            T* newDecData= QoZ::external_wavelet_postprocessing<T,N>(decData, conf.coeffs_dims, conf.coeffs_num, conf.wavelet, conf.pid, false,conf.dims);

            //std::cout<<conf.coeffs_num<<std::endl;

            //std::cout<<conf.coeffs_dims[2]<<std::endl;

            //std::cout<<conf.wavelet<<std::endl;

            //std::cout<<conf.dims[2]<<std::endl;
            
          
            delete []decData;
            decData = new T [conf.num];
            //std::cout<<conf.num;
            memcpy(decData,newDecData,sizeof(T)*conf.num);
            delete []newDecData;

           // std::cout<<decData[100000]<<std::endl;


            
        }
        
        else{
            QoZ::Wavelet<T,N> wlt;
            wlt.postProcess_cdf97(decData,conf.dims);
        }
       
        if(conf.conditioning and (!use_sperr<T,N>(conf) or conf.wavelet>1)){
            auto rtn=post_Condition<T,N>(decData,conf.num,conf.meta);
                
        }

       
        //QoZ::writefile<T>("waved.qoz.dec.idwt", decData, conf.num);
       
        

        //newconf.blockSize=32768;
        
        if(second>0){
            T *offsets =new T [conf.num];
                QoZ::Config newconf(conf.num);
            outlier_decompress<T,N>(newconf,(char*)(cmpDataPos+first),second,offsets);
        
        
        //QoZ::writefile<T>("waved.qoz.dec.offset", offsets, conf.num); 
        //std::cout<<"x4"<<std::endl;
            for(size_t i=0;i<conf.num;i++)
                decData[i]+=offsets[i];
            delete [] offsets;
        }
        //delete [] cmpDataFirst;
        //delete [] cmpDataSecond;
        
    }    
}


template<class T, QoZ::uint N>
double do_not_use_this_interp_compress_block_test(T *data, std::vector<size_t> dims, size_t num,
                                                  double eb, int interp_op, int direction_op, int block_size) {
    std::vector<T> data1(data, data + num);
    size_t outSize = 0;
    QoZ::Config conf;
    conf.absErrorBound = eb;
    conf.setDims(dims.begin(), dims.end());
    conf.blockSize = block_size;
    conf.interpAlgo = interp_op;
    conf.interpDirection = direction_op;
    auto sz = QoZ::SZBlockInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
            QoZ::LinearQuantizer<T>(eb),
            QoZ::HuffmanEncoder<int>(),
            QoZ::Lossless_zstd());
    char *cmpData = (char *) sz.compress(conf, data1.data(), outSize);
    delete[]cmpData;
    auto compression_ratio = num * sizeof(T) * 1.0 / outSize;
    return compression_ratio;
}

template<class T, QoZ::uint N>
char *SZ_compress_AutoSelectiveInterp(QoZ::Config &conf, T *data, size_t &outSize,std::vector <int> InterpAlgo_Candidates,std::vector <int>interpDirection_Candidates,bool blockwise=false){
    //Assert absError already calculated
    
    size_t element_num=conf.num;   
    if (conf.levelwisePredictionSelection<=1){
        std::vector<T> orig_data(element_num,0);
        //std::vector<int> best_quant_bins;
        //std::vector<T> best_decomp(element_num,0);
        uint8_t best_interpAlgo;
        uint8_t best_interpDirection;
        size_t best_cmpsize=0;
        double best_predloss=std::numeric_limits<double>::max();
        for (int i=0;i<element_num;i++){
            orig_data[i]=data[i];
        }
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                        QoZ::LinearQuantizer<T>(conf.absErrorBound),
                        QoZ::HuffmanEncoder<int>(),
                        QoZ::Lossless_zstd());
        for (auto &interp_op:InterpAlgo_Candidates) {
            for (auto &interp_direction: interpDirection_Candidates) {
                conf.interpAlgo=interp_op;
                conf.interpDirection=interp_direction;
                
                size_t cur_cmpsize;
                
                if (blockwise){
                    double cur_predloss;

                    auto cmprData = sz.compress(conf,data,cur_cmpsize,2);
                    delete []cmprData;
                    cur_predloss=conf.decomp_square_error;
                   
                    if (cur_predloss<best_predloss){
                                               
                        best_predloss=cur_predloss;
                        best_interpAlgo=interp_op;
                        best_interpDirection=interp_direction;
                        //best_quant_bins=conf.quant_bins;

                    }
                }
                else{
                    auto cmprData=sz.compress(conf,data,cur_cmpsize,0);
                    delete []cmprData;
                   
                    if (best_cmpsize==0 or cur_cmpsize<best_cmpsize){
                                                
                        best_cmpsize=cur_cmpsize;
                        best_interpAlgo=interp_op;
                        best_interpDirection=interp_direction;
                        //best_quant_bins=conf.quant_bins;

                    }
                }
                for(int i=0;i<element_num;i++){
                    data[i]=orig_data[i];
                }
            }        
        }
        //delete sz;
        
        conf.interpAlgo=best_interpAlgo;
        conf.interpDirection=best_interpDirection;

            //size_t cur_cmpsize;
        auto sz_2 = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                    QoZ::LinearQuantizer<T>(conf.absErrorBound),
                    QoZ::HuffmanEncoder<int>(),
                    QoZ::Lossless_zstd());
            /*
            return (char*)sz.encoding_lossless(conf,best_quant_bins,cur_cmpsize,true);
            */
        return (char*)sz_2.compress(conf,data,outSize,blockwise?1:0);
    }
    else{//levelwise
        std::vector<T> orig_data(element_num,0);
        //std::vector<int> best_quant_bins;
        //std::vector<T> best_decomp(element_num,0);
        std::vector<uint8_t> best_interpAlgo_list(conf.levelwisePredictionSelection,0);
        std::vector<uint8_t> best_interpDirection_list(conf.levelwisePredictionSelection,0);
        
        for (int i=0;i<element_num;i++){

            orig_data[i]=data[i];
        }
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                            QoZ::LinearQuantizer<T>(conf.absErrorBound),
                            QoZ::HuffmanEncoder<int>(),
                            QoZ::Lossless_zstd());
        for(int start_level=conf.levelwisePredictionSelection;start_level>=1;start_level--){
            double best_loss=std::numeric_limits<double>::max();
            uint8_t best_interpAlgo;
            uint8_t best_interpDirection;
            for (auto &interp_op:InterpAlgo_Candidates) {
                for (auto &interp_direction: interpDirection_Candidates) {
                    conf.interpAlgo=interp_op;
                    conf.interpDirection=interp_direction;
                    size_t cur_cmpsize;
                    
                    auto cmprData = sz.compress(conf,data,cur_cmpsize,2,start_level==conf.levelwisePredictionSelection?9999:start_level,start_level-1);
                    delete []cmprData;
                    double cur_loss=conf.decomp_square_error;
                    if ( cur_loss<best_loss){
                        best_loss=cur_loss;
                        best_interpAlgo=interp_op;
                        best_interpDirection=interp_direction;
                        //best_quant_bins=conf.quant_bins;
                    }

                    for(int i=0;i<element_num;i++){                       
                       data[i]=orig_data[i];
                    }
                }           
            }
            size_t cur_cmpsize;
           
            conf.interpAlgo=best_interpAlgo;
            conf.interpDirection=best_interpDirection;
            best_interpAlgo_list[start_level-1]=best_interpAlgo;
            best_interpDirection_list[start_level-1]=best_interpDirection;
            if(conf.pdTuningRealComp){
                auto cmprData = sz.compress(conf,data,cur_cmpsize,2,start_level==conf.levelwisePredictionSelection?9999:start_level,start_level-1);
                delete []cmprData;
                for (int i=0;i<element_num;i++){
                    orig_data[i]=data[i];
                }
            }
        }
        //delete sz;

        conf.interpAlgo_list=best_interpAlgo_list;
        conf.interpDirection_list=best_interpDirection_list;
        for(int i=0;i<element_num;i++){                       
                        data[i]=orig_data[i];
                    }

        //size_t cur_cmpsize;
        auto sz_2 = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                    QoZ::LinearQuantizer<T>(conf.absErrorBound),
                    QoZ::HuffmanEncoder<int>(),
                    QoZ::Lossless_zstd());
        return (char*)sz_2.compress(conf,data,outSize,blockwise?1:0);       
    }
}

template<class T, QoZ::uint N>
char *SZ_compress_AutoSelectiveInterp_with_sampling(QoZ::Config &conf, T *data, size_t &outSize,std::vector <int> InterpAlgo_Candidates,std::vector <int>interpDirection_Candidates,size_t sample_size,bool blockwise=false){
    //Assert absError already calculated

    size_t  global_num=conf.num;
    std::vector<size_t> global_dims=conf.dims;
    std::vector<T> sampled_data;

    if(N==2){
        conf.dims=std::vector<size_t>{sample_size+1,sample_size+1};
        conf.num=(sample_size+1)*(sample_size+1);
        std::vector<size_t> starts{0,0};
        QoZ::sample_blocks<T,N>(data,sampled_data,conf.dims,starts,sample_size+1);
    }
    else{//N==3
        conf.dims=std::vector<size_t>{sample_size+1,sample_size+1,sample_size+1};
        conf.num=(sample_size+1)*(sample_size+1)*(sample_size+1);
        std::vector<size_t> starts{0,0,0};
        QoZ::sample_blocks<T,N>(data,sampled_data,conf.dims,starts,sample_size+1);
 
    }
              
    std::vector<T> orig_sampled_data(conf.num,0);
    for (int i=0;i<conf.num;i++){
        orig_sampled_data[i]=sampled_data[i];
    }
    
    if (conf.levelwisePredictionSelection<=1){       
        //std::vector<int> best_quant_bins;
        //std::vector<T> best_decomp(element_num,0);
        uint8_t best_interpAlgo;
        uint8_t best_interpDirection;
        size_t best_cmpsize=0;
        double best_predloss=std::numeric_limits<double>::max();
        
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                        QoZ::LinearQuantizer<T>(conf.absErrorBound),
                        QoZ::HuffmanEncoder<int>(),
                        QoZ::Lossless_zstd());
        for (auto &interp_op:InterpAlgo_Candidates) {
            for (auto &interp_direction: interpDirection_Candidates) {
                conf.interpAlgo=interp_op;
                conf.interpDirection=interp_direction;              
                size_t cur_cmpsize;              
                double cur_predloss;
                auto cmprData = sz.compress(conf,sampled_data.data(),cur_cmpsize,2);
                delete []cmprData;
                cur_predloss=conf.decomp_square_error;
                   
                if (cur_predloss<best_predloss){                       
                    best_predloss=cur_predloss;
                    best_interpAlgo=interp_op;
                    best_interpDirection=interp_direction;
                        //best_quant_bins=conf.quant_bins;
                }                
                for(int i=0;i<conf.num;i++){                  
                    sampled_data[i]=orig_sampled_data[i];
                }
            }        
        }
        //delete sz;        
        conf.interpAlgo=best_interpAlgo;
        conf.interpDirection=best_interpDirection;
        conf.dims=global_dims;
        conf.num=global_num;

            //size_t cur_cmpsize;
        auto sz_2 = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                    QoZ::LinearQuantizer<T>(conf.absErrorBound),
                    QoZ::HuffmanEncoder<int>(),
                    QoZ::Lossless_zstd());
            /*
            return (char*)sz.encoding_lossless(conf,best_quant_bins,cur_cmpsize,true);
            */
        return (char*)sz_2.compress(conf,data,outSize,blockwise?1:0);        
    }
    else{//levelwise
       
        //std::vector<int> best_quant_bins;
        //std::vector<T> best_decomp(element_num,0);
        std::vector<uint8_t> best_interpAlgo_list(conf.levelwisePredictionSelection,0);
        std::vector<uint8_t> best_interpDirection_list(conf.levelwisePredictionSelection,0);        
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                            QoZ::LinearQuantizer<T>(conf.absErrorBound),
                            QoZ::HuffmanEncoder<int>(),
                            QoZ::Lossless_zstd());
        for(int start_level=conf.levelwisePredictionSelection;start_level>=1;start_level--){
            double best_loss=std::numeric_limits<double>::max();
            uint8_t best_interpAlgo;
            uint8_t best_interpDirection;
            for (auto &interp_op:InterpAlgo_Candidates) {
                for (auto &interp_direction: interpDirection_Candidates) {
                    conf.interpAlgo=interp_op;
                    conf.interpDirection=interp_direction;
                    size_t cur_cmpsize;
                    auto cmprData = sz.compress(conf,sampled_data.data(),cur_cmpsize,2,start_level==conf.levelwisePredictionSelection?9999:start_level,start_level-1);
                    delete []cmprData;
                    double cur_loss=conf.decomp_square_error;
                    if ( cur_loss<best_loss){
                        best_loss=cur_loss;
                        best_interpAlgo=interp_op;
                        best_interpDirection=interp_direction;
                        //best_quant_bins=conf.quant_bins;
                    }

                    for(int i=0;i<conf.num;i++){
                        sampled_data[i]=orig_sampled_data[i];
                    }
                }
            }
            size_t cur_cmpsize;           
            conf.interpAlgo=best_interpAlgo;
            conf.interpDirection=best_interpDirection;
            best_interpAlgo_list[start_level-1]=best_interpAlgo;
            best_interpDirection_list[start_level-1]=best_interpDirection;
            if(conf.pdTuningRealComp){
                auto cmprData = sz.compress(conf,sampled_data.data(),cur_cmpsize,2,start_level==conf.levelwisePredictionSelection?9999:start_level,start_level-1);
                delete []cmprData;
                for (int i=0;i<conf.num;i++){
                    orig_sampled_data[i]=sampled_data[i];
                }
            }           
        }
        //delete sz;
        conf.interpAlgo_list=best_interpAlgo_list;
        conf.interpDirection_list=best_interpDirection_list;
        conf.dims=global_dims;
        conf.num=global_num;        
        //size_t cur_cmpsize;
        auto sz_2 = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                    QoZ::LinearQuantizer<T>(conf.absErrorBound),
                    QoZ::HuffmanEncoder<int>(),
                    QoZ::Lossless_zstd());
        return (char*)sz_2.compress(conf,data,outSize,blockwise?1:0);        
    }
}


template<class T, QoZ::uint N>
inline void init_alphalist(std::vector<double> &alpha_list,const double &rel_bound, QoZ::Config &conf){

    if (use_sperr<T,N>(conf))
    {
        alpha_list={1};
        return;
    }
    if(conf.linearReduce){
        alpha_list={0,0.1,0.2,0.3,0.4,0.5};

    }
    else{
        if (conf.tuningTarget!=QoZ::TUNING_TARGET_CR){
            if(conf.wavelet==0){
                if(conf.abList==0)            
                    alpha_list={1,1.25,1.5,1.75,2};
                else if(conf.abList==1)
                    alpha_list={1,1.25,1.5,1.75,2,2.25,2.5};
                else
                    alpha_list={1,1.25,1.5,1.75,2,2.25,2.5,2.75,3};
            }
            else{
                alpha_list={1.5,1.75};
            }
        }
        else{
            if(conf.wavelet==0){
                alpha_list={-1,1,1.25,1.5,1.75,2};
            }
            else
                alpha_list={-1,1,1.25,1.5};
        }
    }
}
template<class T, QoZ::uint N>
inline void init_betalist(std::vector<double> &beta_list,const double &rel_bound, QoZ::Config &conf){
    if (use_sperr<T,N>(conf))
    {
        beta_list={1};
        return;
    }
    if(conf.linearReduce){
        beta_list={1,0.75,0.5,0.33,0.25};
    }
    else{
        if (conf.tuningTarget!=QoZ::TUNING_TARGET_CR){    
            if(conf.wavelet==0){        
                beta_list={1.5,2,3,4};
            }
            else{
                beta_list={2,3};
            }
        }
        else {
            if(conf.wavelet==0)
                beta_list={-1,1.5,2,3};
            else
                 beta_list={-1,1.5,2};
        }
    }
}

template<class T, QoZ::uint N>
inline void init_gammalist(std::vector<double> &gamma_list,const double &rel_bound, QoZ::Config &conf){
    if (use_sperr<T,N>(conf))
    {   
        if(conf.wavelet==1)
            gamma_list={0.5,0.75,1,1.25,1.5};
        else
            gamma_list={0.5,0.75,1,1.25,1.5};
            //gamma_list={1.5,3,5,10,20};
       
    }
   
    else{
        if(conf.wavelet==0)
            gamma_list={1};
        else
            gamma_list={0.5,0.75,1,1.25,1.5};
        
    }
}
/*
template<class T, QoZ::uint N>
int compareWavelets(QoZ::Config &conf, std::vector< std::vector<T> > & sampled_blocks){
    size_t sampleBlockSize=conf.sampleBlockSize;
    std::vector<size_t> global_dims=conf.dims;
    size_t global_num=conf.num;

    num_sampled_blocks=sampled_blocks.size();
    per_block_ele_num=pow(sampleBlockSize+1,N);
    ele_num=num_sampled_blocks*per_block_ele_num;
    conf.dims=std::vector<size_t>(N,sampleBlockSize+1);
    conf.num=per_block_ele_num;
    std::vector<T> cur_block(per_block_ele_num,0);

    double wave_eb=conf.absErrorBound*conf.wavelet_rel_coeff;

    size_t sig_count=0;

    std::vector<T> gathered_coeffs;
    std::vector<T> gathered_blocks;

    return 0;

}
*/


template<class T, QoZ::uint N>
void sampleBlocks(T *data,std::vector<size_t> &dims, size_t sampleBlockSize,std::vector< std::vector<T> > & sampled_blocks,double sample_rate,int profiling ,std::vector<std::vector<size_t> > &starts,int var_first=0){
    for(int i=0;i<sampled_blocks.size();i++){
                std::vector< T >().swap(sampled_blocks[i]);                
            }
            std::vector< std::vector<T> >().swap(sampled_blocks);
    for(int i=0;i<sampled_blocks.size();i++){
        std::vector< T >().swap(sampled_blocks[i]);                  
    }
    std::vector< std::vector<T> >().swap(sampled_blocks);                               
    size_t totalblock_num=1;
    for(int i=0;i<N;i++){                        
        totalblock_num*=(int)((dims[i]-1)/sampleBlockSize);
    }               
    size_t idx=0,block_idx=0;   
    if(profiling){
        size_t num_filtered_blocks=starts.size();    
        if(var_first==0){  
            size_t sample_stride=(size_t)(num_filtered_blocks/(totalblock_num*sample_rate));
            if(sample_stride<=0)
                sample_stride=1;
            
            for(size_t i=0;i<num_filtered_blocks;i+=sample_stride){
                std::vector<T> s_block;
                QoZ::sample_blocks<T,N>(data, s_block,dims, starts[i],sampleBlockSize+1);
                sampled_blocks.push_back(s_block);
                
            }
            
        }
        else{
            std::vector< std::pair<double,std::vector<size_t> > >block_heap;
            for(size_t i=0;i<num_filtered_blocks;i++){
                double mean,sigma2,range;
                QoZ::blockwise_profiling<T>(data,dims, starts[i],sampleBlockSize+1, mean,sigma2,range);
                block_heap.push_back(std::pair<double,std::vector<size_t> >(sigma2,starts[i]));
            }
            std::make_heap(block_heap.begin(),block_heap.end());
            size_t sampled_block_num=totalblock_num*sample_rate;
            if(sampled_block_num==0)
                sampled_block_num=1;
            for(size_t i=0;i<sampled_block_num;i++){
                std::vector<T> s_block;
                QoZ::sample_blocks<T,N>(data, s_block,dims, block_heap.front().second,sampleBlockSize+1);
                
                sampled_blocks.push_back(s_block);
                std::pop_heap(block_heap.begin(),block_heap.end());
                block_heap.pop_back();
            }
        }
    }               
    else{
        if(var_first==0){
            size_t sample_stride=(size_t)(1.0/sample_rate);
            if(sample_stride<=0)
                sample_stride=1;
            if (N==2){                        
                //std::vector<size_t> sample_dims(2,sampleBlockSize+1);
                for (size_t x_start=0;x_start<dims[0]-sampleBlockSize;x_start+=sampleBlockSize){                           
                    for (size_t y_start=0;y_start<dims[1]-sampleBlockSize;y_start+=sampleBlockSize){
                        if (idx%sample_stride==0){
                            std::vector<size_t> starts{x_start,y_start};
                            std::vector<T> s_block;
                            QoZ::sample_blocks<T,N>(data, s_block,dims, starts,sampleBlockSize+1);
                            sampled_blocks.push_back(s_block);
                        }
                        idx+=1;
                    }
                }
            }
            else if (N==3){
                //std::vector<size_t> sample_dims(3,sampleBlockSize+1);                     
                for (size_t x_start=0;x_start<dims[0]-sampleBlockSize;x_start+=sampleBlockSize){                          
                    for (size_t y_start=0;y_start<dims[1]-sampleBlockSize;y_start+=sampleBlockSize){
                        for (size_t z_start=0;z_start<dims[2]-sampleBlockSize;z_start+=sampleBlockSize){
                            if (idx%sample_stride==0){
                                std::vector<size_t> starts{x_start,y_start,z_start};
                                std::vector<T> s_block;
                                QoZ::sample_blocks<T,N>(data, s_block,dims, starts,sampleBlockSize+1);
                                sampled_blocks.push_back(s_block);
                            }
                            idx+=1;
                        }
                    }
                }
            }
        }
        else{
            std::vector <std::vector<size_t> > blocks_starts;
            if (N==2){  
                for (size_t x_start=0;x_start<dims[0]-sampleBlockSize;x_start+=sampleBlockSize){                           
                    for (size_t y_start=0;y_start<dims[1]-sampleBlockSize;y_start+=sampleBlockSize){
                       
                            blocks_starts.push_back(std::vector<size_t>{x_start,y_start});
                    }
                }

            }
            else if (N==3){
                //std::vector<size_t> sample_dims(3,sampleBlockSize+1);                     
                for (size_t x_start=0;x_start<dims[0]-sampleBlockSize;x_start+=sampleBlockSize){                          
                    for (size_t y_start=0;y_start<dims[1]-sampleBlockSize;y_start+=sampleBlockSize){
                        for (size_t z_start=0;z_start<dims[2]-sampleBlockSize;z_start+=sampleBlockSize){
                            blocks_starts.push_back(std::vector<size_t>{x_start,y_start,z_start});
                        }
                    }
                }
            

                std::vector< std::pair<double,std::vector<size_t> > >block_heap;
                for(size_t i=0;i<totalblock_num;i++){
                    double mean,sigma2,range;
                    QoZ::blockwise_profiling<T>(data,dims, blocks_starts[i],sampleBlockSize+1, mean,sigma2,range);
                    block_heap.push_back(std::pair<double,std::vector<size_t> >(sigma2,blocks_starts[i]));
                }
                std::make_heap(block_heap.begin(),block_heap.end());
                size_t sampled_block_num=totalblock_num*sample_rate;
                if(sampled_block_num==0)
                    sampled_block_num=1;
                for(size_t i=0;i<sampled_block_num;i++){
                    std::vector<T> s_block;
                    QoZ::sample_blocks<T,N>(data, s_block,dims, block_heap.front().second,sampleBlockSize+1);
                    sampled_blocks.push_back(s_block);
                    std::pop_heap(block_heap.begin(),block_heap.end());
                    block_heap.pop_back();
                }

            }
        }
    }
}


template<class T, QoZ::uint N>
std::pair<double,double> CompressTest(const QoZ::Config &conf,const std::vector< std::vector<T> > & sampled_blocks,QoZ::ALGO algo = QoZ::ALGO_INTERP,
                    QoZ::TUNING_TARGET tuningTarget=QoZ::TUNING_TARGET_RD,bool useFast=true,double profiling_coeff=1,const std::vector<double> &orig_means=std::vector<double>(),
                    const std::vector<double> &orig_sigma2s=std::vector<double>(),const std::vector<double> &orig_ranges=std::vector<double>(),const std::vector<T> &flattened_sampled_data=std::vector<T>(),const std::vector< std::vector<T> > & waveleted_input=std::vector< std::vector<T> >()){
    QoZ::Config testConfig(conf);
    size_t ssim_size=conf.SSIMBlockSize;    
    if(algo == QoZ::ALGO_LORENZO_REG){
        testConfig.cmprAlgo = QoZ::ALGO_LORENZO_REG;
        testConfig.dims=conf.dims;
        testConfig.num=conf.num;
        //lorenzo_config.setDims(sample_dims.begin(), sample_dims.end());
        testConfig.lorenzo = true;
        testConfig.lorenzo2 = true;
        testConfig.regression = false;
        testConfig.regression2 = false;
        testConfig.openmp = false;
        testConfig.blockSize = 5;//why?
        testConfig.quantbinCnt = 65536 * 2;
    }
    double square_error=0.0;
    double bitrate=0.0;
    double metric=0.0;
    size_t sampleBlockSize=testConfig.sampleBlockSize;
    size_t num_sampled_blocks=sampled_blocks.size();
    size_t per_block_ele_num=pow(sampleBlockSize+1,N);
    size_t ele_num=num_sampled_blocks*per_block_ele_num;
    std::vector<T> cur_block(testConfig.num,0);
    std::vector<int> q_bins;
    std::vector<std::vector<int> > block_q_bins;
    std::vector<size_t> q_bin_counts;
    std::vector<T> flattened_cur_blocks;
    //char *cmpData;
    size_t idx=0;   
    QoZ::concepts::CompressorInterface<T> *sz;
    size_t totalOutSize=0;
    if(algo == QoZ::ALGO_LORENZO_REG){
        auto quantizer = QoZ::LinearQuantizer<T>(testConfig.absErrorBound, testConfig.quantbinCnt / 2);
        if (useFast &&N == 3 && !testConfig.regression2) {
            sz = QoZ::make_sz_general_compressor<T, N>(QoZ::make_sz_fast_frontend<T, N>(testConfig, quantizer), QoZ::HuffmanEncoder<int>(),
                                                                   QoZ::Lossless_zstd());
        }
        else{
            sz = make_lorenzo_regression_compressor<T, N>(testConfig, quantizer, QoZ::HuffmanEncoder<int>(), QoZ::Lossless_zstd());

        }
    }
    else if(algo == QoZ::ALGO_INTERP){

        sz =  new QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                        QoZ::LinearQuantizer<T>(testConfig.absErrorBound),
                        QoZ::HuffmanEncoder<int>(),
                        QoZ::Lossless_zstd());

    }
    else{
        std::cout<<"algo type error!"<<std::endl;
        return std::pair<double,double>(0,0);
    }
                           
    for (int k=0;k<num_sampled_blocks;k++){
        size_t sampleOutSize;
        std::vector<T> cur_block(testConfig.num);
        if(testConfig.wavelet==0 or waveleted_input.size()==0){
            std::cout<<"fuqindejian0"<<std::endl;
            std::copy(sampled_blocks[k].begin(),sampled_blocks[k].end(),cur_block.begin());
        }
        else{
            std::copy(waveleted_input[k].begin(),waveleted_input[k].end(),cur_block.begin());
            std::cout<<"fuqindejian1"<<std::endl;

        }
        char *cmprData;
        if(use_sperr<T,N>(testConfig)){
            if(testConfig.wavelet<=1){
                cmprData=SPERR_Compress<T,N>(testConfig,cur_block.data(),sampleOutSize);
                totalOutSize+=sampleOutSize;
                //std::cout<<sampleOutSize<<std::endl;
                if(tuningTarget!=QoZ::TUNING_TARGET_CR){
                    SPERR_Decompress<T,N>(cmprData,sampleOutSize,cur_block.data());
                    
                } 
            }
            else{
                //for (size_t i=0;i<N;i++)
                   // std::cout<<testConfig.dims[i]<<" ";
                //std::cout<<cur_block.size()<<std::endl;
                cmprData=SPERR_Compress<T,N>(testConfig,cur_block.data(),sampleOutSize);
                //std::vector<T> sperr_wave_dec(cur_block.size());
                //std::cout<<sampleOutSize<<std::endl;
                totalOutSize+=sampleOutSize;
                if(1){//tuningTarget!=QoZ::TUNING_TARGET_CR){
                    SPERR_Decompress<T,N>(cmprData,sampleOutSize,cur_block.data());
                    std::vector<size_t> ori_sbs(N,testConfig.sampleBlockSize+1);
                    T *idwtData=QoZ::external_wavelet_postprocessing<T,N>(cur_block.data(),testConfig.dims, testConfig.num, testConfig.wavelet, testConfig.pid, false,ori_sbs);
                    

                    //std::cout<<"fuqindejian3"<<std::endl;     

                    cur_block.resize(per_block_ele_num);
                    for(size_t i=0;i<per_block_ele_num;i++)
                        cur_block[i]=idwtData[i];
                    delete []idwtData;
                    if(testConfig.conditioning){
                        post_Condition<T,N>(cur_block.data(),per_block_ele_num,testConfig.block_metas[k]);
                    }
                    
                    std::vector<T> offsets(per_block_ele_num);
                    
                    for(size_t i=0;i<per_block_ele_num;i++)
                        offsets[i]=sampled_blocks[k][i]-cur_block[i];
                    
                    size_t oc_size;
                    std::vector<size_t> ori_dims=testConfig.dims,temp_dims={per_block_ele_num};

                    testConfig.setDims(temp_dims.begin(),temp_dims.end());

                    char * offsetsCmprData=outlier_compress<T,N>(testConfig,offsets.data(),oc_size);
                    testConfig.setDims(ori_dims.begin(),ori_dims.end());
                    delete []offsetsCmprData;
                    totalOutSize+=oc_size;
                    //std::cout<<oc_size<<std::endl;
                    for(size_t i=0;i<per_block_ele_num;i++)
                        cur_block[i]+=offsets[i];

                }
                



            }
   
            delete []cmprData;          
        }    
        else{
            cmprData = (char*)sz->compress(testConfig, cur_block.data(), sampleOutSize,1);
            //std::cout<<"fuqindejian2"<<std::endl;     
            delete[]cmprData;
            if(testConfig.wavelet>0 and waveleted_input.size()>0 and tuningTarget!=QoZ::TUNING_TARGET_CR){
                //std::cout<<"test with wave"<<std::endl;
                if(testConfig.wavelet==1){
                    QoZ::Wavelet<T,N> wlt;
                    wlt.postProcess_cdf97(cur_block.data(),conf.dims);
                    
                    //std::cout<<"fuqindejian"<<std::endl; 
                }
                else{
                    std::vector<size_t> ori_sbs(N,testConfig.sampleBlockSize+1);
                    T *idwtData=QoZ::external_wavelet_postprocessing<T,N>(cur_block.data(),testConfig.dims, testConfig.num, testConfig.wavelet, testConfig.pid, false,ori_sbs);
                    //std::cout<<"fuqindejian3"<<std::endl;     

                    cur_block.resize(per_block_ele_num);
                    for(size_t i=0;i<per_block_ele_num;i++)
                        cur_block[i]=idwtData[i];
                    delete []idwtData;
                    //std::cout<<"fuqindejian4"<<std::endl;  
                }
                if(testConfig.conditioning){
                    
                    post_Condition<T,N>(cur_block.data(),per_block_ele_num,testConfig.block_metas[k]);
                }

            }
        }

        
        if(algo==QoZ::ALGO_INTERP and !(use_sperr<T,N>(testConfig)))
            block_q_bins.push_back(testConfig.quant_bins);

        if(tuningTarget==QoZ::TUNING_TARGET_RD){
            if(algo==QoZ::ALGO_INTERP and !(use_sperr<T,N>(testConfig)) )
                square_error+=testConfig.decomp_square_error;
            else{
                //T maxval=0;
                for(size_t j=0;j<per_block_ele_num;j++){
                    T value=sampled_blocks[k][j]-cur_block[j];
                    square_error+=value*value;
                    //maxval=fabs(value)>maxval?fabs(value):maxval;
                }
                //std::cout<<maxval<<std::endl;
                //std::cout<<square_error<<std::endl;
            }
        }
        else if (tuningTarget==QoZ::TUNING_TARGET_SSIM){

            size_t ssim_block_num=orig_means.size();                           
            double mean=0,sigma2=0,cov=0,range=0;
            double orig_mean=0,orig_sigma2=0,orig_range=0;                        
            if(N==2){
                for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                    for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                        orig_mean=orig_means[idx];
                        orig_sigma2=orig_sigma2s[idx];
                        orig_range=orig_ranges[idx];
                        std::vector<size_t> starts{i,j};
                        QoZ::blockwise_profiling<T>(cur_block.data(),testConfig.dims,starts,ssim_size,mean,sigma2,range);
                        cov=QoZ::blockwise_cov<T>(sampled_blocks[k].data(),cur_block.data(),testConfig.dims,starts,ssim_size,orig_mean,mean);
                        metric+=QoZ::SSIM(orig_range,orig_mean,orig_sigma2,mean,sigma2,cov)/ssim_block_num;
                        idx++;


                    }
                }
            }
            else if(N==3){
                for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                    for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                        for (size_t kk=0;kk+ssim_size<sampleBlockSize+1;kk+=ssim_size){
                            orig_mean=orig_means[idx];
                            orig_sigma2=orig_sigma2s[idx];
                            orig_range=orig_ranges[idx];
                            std::vector<size_t> starts{i,j,kk};
                            QoZ::blockwise_profiling<T>(cur_block.data(),testConfig.dims,starts,ssim_size,mean,sigma2,range);
                                            
                            cov=QoZ::blockwise_cov<T>(sampled_blocks[k].data(),cur_block.data(),testConfig.dims,starts,ssim_size,orig_mean,mean);
                                            //printf("%.8f %.8f %.8f %.8f %.8f %.8f %.8f\n",orig_range,orig_sigma2,orig_mean,range,sigma2,mean,cov);
                            metric+=QoZ::SSIM(orig_range,orig_mean,orig_sigma2,mean,sigma2,cov)/ssim_block_num;
                            idx++;
                        }
                    }
                }
            }
        }
        else if (tuningTarget==QoZ::TUNING_TARGET_AC){
            flattened_cur_blocks.insert(flattened_cur_blocks.end(),cur_block.begin(),cur_block.end());
        }                      
    }
    if(algo==QoZ::ALGO_INTERP and !(use_sperr<T,N>(testConfig))){
        q_bin_counts=testConfig.quant_bin_counts;
        size_t level_num=q_bin_counts.size();
        size_t last_pos=0;
        for(int k=level_num-1;k>=0;k--){
            for (size_t l =0;l<num_sampled_blocks;l++){
                for (size_t m=last_pos;m<q_bin_counts[k];m++){
                    q_bins.push_back(block_q_bins[l][m]);
                }
            }
            last_pos=q_bin_counts[k];
        }      
    }
    size_t sampleOutSize;
    //std::cout<<q_bins.size()<<std::endl;
    //std::cout<<"muqindedao0.0"<<std::endl; 
    if(!use_sperr<T,N>(testConfig)){
        auto cmprData=sz->encoding_lossless(totalOutSize,q_bins);                   
        delete[]cmprData;   
       
    }  
    //std::cout<<"muqindedao"<<std::endl;         
   
    bitrate=8*double(totalOutSize)/ele_num;
    
    bitrate*=profiling_coeff;
                    //std::cout<<bitrate<<std::endl;
    
                //bitrate+=8*sizeof(T)*anchor_rate;//added
                /*
                if (conf.tuningTarget==QoZ::TUNING_TARGET_SSIM){
                     mean=sum/ele_num;
                    sigma=sqrt((square_sum/ele_num)-(mean*mean));
                    cov=(covsum/ele_num)-mean*orig_mean;

                    printf("%.4f %.8f %.8f %.8f %.8f %.8f\n",rng,orig_mean,orig_sigma,mean,sigma,cov);


                      
                    metric=QoZ::SSIM(rng,rng,orig_mean,orig_sigma,mean,sigma,cov);


                }
 
                */
    if(tuningTarget==QoZ::TUNING_TARGET_RD){
                    //std::cout<<"---"<<std::endl;
        double mse=square_error/ele_num;
                   // std::cout<<mse<<std::endl;      
        mse*=profiling_coeff;      
        if(testConfig.wavelet==1)
            mse*=testConfig.waveletMseFix;
        else if(testConfig.wavelet>1)
            mse*=testConfig.waveletMseFix2;
                    //std::cout<<mse<<std::endl;
        metric=QoZ::PSNR(testConfig.rng,mse);
                    //std::cout<<metric<<std::endl;
                    //std::cout<<"---"<<std::endl;
    }
    else if (tuningTarget==QoZ::TUNING_TARGET_AC){                       
        metric=1.0-QoZ::autocorrelation<T>(flattened_sampled_data.data(),flattened_cur_blocks.data(),ele_num);                        
    }                    
            //printf("%.2f %.2f %.4f %.2f\n",testConfig.alpha,testConfig.beta,bitrate,metric);   
    if(testConfig.wavelet==1){
        bitrate*=testConfig.waveletBrFix;
    } 
    else if(testConfig.wavelet>1){
        bitrate*=testConfig.waveletBrFix2;
    }           
    delete sz;
    return std::pair(bitrate,metric);
}

std::pair <double,double> setABwithRelBound(double rel_bound,int configuration=0){

    double cur_alpha=-1,cur_beta=-1;
    if(configuration==0){              
        if (rel_bound>=0.01){
            cur_alpha=2;
            cur_beta=2;
        }
        else if (rel_bound>=0.007){
            cur_alpha=1.75;
            cur_beta=2;
        }                 
        else if (rel_bound>=0.004){
            cur_alpha=1.5;
            cur_beta=2;
        }                   
        else if (rel_bound>0.001){
            cur_alpha=1.25;
            cur_beta=1.5;
        }
        else {
            cur_alpha=1;
            cur_beta=1;
        }
    }
    else if(configuration==1){                
        if (rel_bound>=0.01){
            cur_alpha=2;
            cur_beta=4;
        }
        else if (rel_bound>=0.007){
            cur_alpha=1.75;
            cur_beta=3;
        }
        else if (rel_bound>=0.004){
            cur_alpha=1.5;
            cur_beta=2;
        }           
        else if (rel_bound>0.001){
            cur_alpha=1.25;
            cur_beta=1.5;
        }
        else {
                cur_alpha=1;
                cur_beta=1;
            }
    }
    else if(configuration==2){                
        if (rel_bound>=0.01){
            cur_alpha=2;
            cur_beta=4;
        }
        else if (rel_bound>=0.007){
            cur_alpha=1.75;
            cur_beta=3;
        }                    
        else if (rel_bound>=0.004){
            cur_alpha=1.5;
            cur_beta=2;
        }
        else if (rel_bound>0.001){
            cur_alpha=1.5;
            cur_beta=1.5;
        }
        else if (rel_bound>0.0005){
            cur_alpha=1.25;
            cur_beta=1.5;
        }
        else {
            cur_alpha=1;
            cur_beta=1;
        }
    }
    return std::pair<double,double>(cur_alpha,cur_beta);
}

void setFixRates(QoZ::Config &conf,double rel_bound){
    if(conf.sperr>=1){
        double e1=1e-3;
        double e2=1e-2;
        double e3=1e-1;
        double f1=1;
        double f2=0.8;
        double f3=0.6;
        if(rel_bound<=e1)
            conf.waveletBrFix=e1;
        else if(rel_bound<=e2)
            conf.waveletBrFix=f1-(f1-f2)*(rel_bound-e1)/(e2-e1);
        else if (rel_bound<=e3)
            conf.waveletBrFix=f2-(f2-f3)*(rel_bound-e2)/(e3-e2);
        else 
            conf.waveletBrFix=f3;
        conf.waveletBrFix=1.0;
        conf.waveletMseFix=1.0;
    }
    else{
        conf.waveletBrFix=0.9;
        conf.waveletMseFix=1.0;
    }
    if(conf.sperr>=2){
        conf.waveletBrFix2=0.1;//Currently it is only for just select 2.
        conf.waveletMseFix2=1;
    }
    else{
        conf.waveletBrFix2=0.1;//Currently it is only for just select 2.
        conf.waveletMseFix2=1;
        /*
        conf.waveletMseFix2=0.9;
        if (rel_bound>=1e-3){
            conf.waveletBrFix2=0.6;
        }
        else if (rel_bound<=1e-4){
            conf.waveletBrFix2=0.55;

        }
        else{
            conf.waveletBrFix2=0.55+0.05*(rel_bound-1e-4)/(1e-3-1e-4);
        }
        */
    }

}

template<class T, QoZ::uint N>
double Tuning(QoZ::Config &conf, T *data){
   
    T rng=conf.rng;
    double rel_bound = conf.relErrorBound>0?conf.relErrorBound:conf.absErrorBound/rng;
    if(rel_bound>1e-3)
        conf.testLorenzo=0;
    //QoZ::Timer timer(true);
    //timer.stop("")
    if(conf.QoZ){
        if(conf.autoTuningRate<=0)
            conf.autoTuningRate = (N==2?0.01:0.005);
        if(conf.predictorTuningRate<=0)
            conf.predictorTuningRate = (N==2?0.01:0.005);
        if (conf.maxStep<=0)
            conf.maxStep = (N==2?64:32);
        if (conf.levelwisePredictionSelection<=0)
            conf.levelwisePredictionSelection = (N==2?6:4);
        if (conf.sampleBlockSize<=0)
            conf.sampleBlockSize = (N==2?64:32);

    }   
    if(conf.waveletAutoTuning>0 and conf.waveAutoFix)
        setFixRates(conf,rel_bound);
    size_t sampling_num, sampling_block;
    double best_interp_cr=0.0;
    double best_lorenzo_ratio=0.0;
    bool useInterp=true;        
    std::vector<size_t> sample_dims(N);
    std::vector<T> sampling_data;
    double anchor_rate=0;
    int max_interp_level = -1;
    int bestWave=0;

    for (size_t i = 0; i < N; i++) {
        if ( max_interp_level < ceil(log2(conf.dims[i]))) {
             max_interp_level = (uint) ceil(log2(conf.dims[i]));
        }
                
    }

    if (conf.maxStep>0){
        anchor_rate=1/(pow(conf.maxStep,N));   
        int temp_max_interp_level=(uint)log2(conf.maxStep);//to be catious: the max_interp_level is different from the ones in szinterpcompressor, which includes the level of anchor grid.
        if (temp_max_interp_level<=max_interp_level){                  
            max_interp_level=temp_max_interp_level;
        }
        if (conf.levelwisePredictionSelection>max_interp_level)
            conf.levelwisePredictionSelection=max_interp_level;
    }
            
    std::vector< std::vector<T> > sampled_blocks;
    size_t sampleBlockSize=conf.sampleBlockSize;
    size_t num_sampled_blocks;
    size_t per_block_ele_num;
    size_t ele_num;
    if (sampleBlockSize<=0){
        sampleBlockSize = (N==2?64:32);
            /*
            if (conf.maxStep>sampleBlockSize)
                sampleBlockSize=conf.maxStep;
                */
    }

    std::vector<int> op_candidates={QoZ::INTERP_ALGO_LINEAR,QoZ::INTERP_ALGO_CUBIC};
    std::vector<int> dir_candidates={0,QoZ::factorial(N)-1};

    if(conf.multiDimInterp){
        dir_candidates.push_back(QoZ::factorial(N));
    }

    std::vector<std::vector<uint8_t> > interpAlgo_lists(conf.waveletAutoTuning+1);
    std::vector<std::vector<uint8_t> > interpDirection_lists(conf.waveletAutoTuning+1);
    std::vector<uint8_t> bestInterpAlgos(conf.waveletAutoTuning+1);
    std::vector<uint8_t> bestInterpDirections(conf.waveletAutoTuning+1);
           
    size_t totalblock_num=1;  
    for(int i=0;i<N;i++){                      
        totalblock_num*=(size_t)((conf.dims[i]-1)/sampleBlockSize);
    }

    std::vector<std::vector<size_t> >starts;
    if((conf.waveletTuningRate>0 or conf.autoTuningRate>0 or conf.predictorTuningRate>0) and conf.profiling){      
        if(N==2){
            QoZ::profiling_block_2d<T,N>(data,conf.dims,starts,sampleBlockSize,conf.absErrorBound,conf.profStride);
        }
        else if (N==3){
            QoZ::profiling_block_3d<T,N>(data,conf.dims,starts,sampleBlockSize,conf.absErrorBound,conf.profStride);
        }
        //num_blocks=starts.size();
    }


    size_t num_filtered_blocks=starts.size();
    double profiling_coeff=1;
    if(conf.profiling)
        profiling_coeff=((double)num_filtered_blocks)/(totalblock_num);
    std::vector<size_t> global_dims=conf.dims;
    size_t global_num=conf.num;

    if(conf.waveletTest>0){        
        if(conf.waveletTuningRate==0)
            conf.waveletTuningRate=conf.predictorTuningRate;
        sampleBlocks<T,N>(data,conf.dims,sampleBlockSize,sampled_blocks,conf.waveletTuningRate,0,starts);         
        num_sampled_blocks=sampled_blocks.size();
        per_block_ele_num=pow(sampleBlockSize+1,N);
        ele_num=num_sampled_blocks*per_block_ele_num;
        conf.dims=std::vector<size_t>(N,sampleBlockSize+1);
        conf.num=per_block_ele_num;
        std::vector<T> cur_block(per_block_ele_num,0);
        double wave_eb=conf.absErrorBound*conf.wavelet_rel_coeff;
        size_t sig_count=0;
        std::vector<T> gathered_coeffs;
        std::vector<T> gathered_blocks;
        for (int i=0;i<num_sampled_blocks;i++){
            cur_block=sampled_blocks[i];
            gathered_blocks.insert(gathered_blocks.end(),cur_block.begin(),cur_block.end());
            QoZ::Wavelet<T,N> wlt;
            //any condition?
            wlt.preProcess_cdf97(cur_block.data(),conf.dims);
            for(size_t i=0;i<conf.num;i++){
                if(fabs(cur_block[i])>wave_eb)
                    sig_count++;
            }
            gathered_coeffs.insert(gathered_coeffs.end(),cur_block.begin(),cur_block.end());
        }
        double sig_rate=(double)sig_count/ele_num;
        double normvar=QoZ::calcNormedVariance(gathered_coeffs.data(),ele_num);
        double orivar=QoZ::calcNormedVariance(gathered_blocks.data(),ele_num);
        std::vector< T >().swap(gathered_coeffs);
        std::vector< T >().swap(gathered_blocks);
        bool useWave=(normvar<1e-4);
        /*
        if (normvar>0.01 or sig_rate>0.05){
            useWave=false;

        }

        if (normvar<1e-4 or sig_rate<0.01){
            useWave=true;

        }*/
        if(conf.verbose){          
            std::cout<<"Sigrate: "<<sig_rate<<" Normvar: "<<normvar<<std::endl;
            std::cout<<"Orivar: "<<orivar<<" Varrate: "<<normvar/orivar<<std::endl;
            std::cout<<"Use wave: "<<useWave<<std::endl;

        }
        conf.dims=global_dims;
        conf.num=global_num;

    }
    if (conf.predictorTuningRate>0 and conf.predictorTuningRate<1){
        //int ori_sperr=conf.sperr;//temp
        //conf.sperr=0;
        if (conf.verbose)
            std::cout<<"Predictor tuning started."<<std::endl;
        double o_alpha=conf.alpha;
        double o_beta=conf.beta;
                    
        if(!conf.waveletTest or conf.predictorTuningRate!=conf.waveletTuningRate or conf.profiling>0){
            sampleBlocks<T,N>(data,conf.dims,sampleBlockSize,sampled_blocks,conf.predictorTuningRate,conf.profiling,starts,conf.var_first);
        }        
        num_sampled_blocks=sampled_blocks.size();
        per_block_ele_num=pow(sampleBlockSize+1,N);
        ele_num=num_sampled_blocks*per_block_ele_num;
        conf.dims=std::vector<size_t>(N,sampleBlockSize+1);
        conf.num=per_block_ele_num;
        std::vector<T> cur_block(per_block_ele_num,0);
     
        //double lorenzo_average_cr=0;
        std::vector <std::vector<T> > ori_sampled_blocks;
        if (conf.waveletAutoTuning>=1)
            ori_sampled_blocks=sampled_blocks;
        for(size_t wave_idx=0;wave_idx<=conf.waveletAutoTuning;wave_idx++){
            if((wave_idx==0 and conf.sperrWithoutWave) or (wave_idx>0 and wave_idx<=conf.sperr))
                continue;
            
            double ori_eb=conf.absErrorBound;
            std::vector<size_t> coeffs_size;
            if(wave_idx>0){//later distinguish different i
                //std::cout<<wave_idx<<std::endl;

               

                conf.absErrorBound*=conf.wavelet_rel_coeff;
                if(conf.conditioning and (!use_sperr<T,N>(conf) or conf.wavelet>1)){
                    //because no decomp,so dont need to save meta and do reverse;
                    for(size_t i=0;i<sampled_blocks.size();i++)
                        auto meta=pre_Condition<T,N>(conf,sampled_blocks[i].data());
                }
                if(wave_idx==1){

                    for(size_t i=0;i<sampled_blocks.size();i++){
                        QoZ::Wavelet<T,N> wlt;
                        wlt.preProcess_cdf97(sampled_blocks[i].data(),conf.dims);

                    }
                }
                else{
                    size_t coeffs_num=1;
                    for(size_t i=0;i<sampled_blocks.size();i++){
                        T *coeffData=QoZ::external_wavelet_preprocessing<T,N>(sampled_blocks[i].data(), conf.dims, conf.num, wave_idx, conf.pid,false,coeffs_size);
                        if(i==0){
                            
                            for (size_t j=0;j<N;j++)
                                coeffs_num*=coeffs_size[j];
                            

                        }
                        sampled_blocks[i].resize(coeffs_num);
                        for (size_t j=0;j<coeffs_num;j++)
                            sampled_blocks[i][j]=coeffData[j]; 
                        delete[]coeffData;
                    }
                    conf.setDims(coeffs_size.begin(),coeffs_size.end());

                }
                //std::cout<<"dawd"<<std::endl;

            }

            if(conf.testLorenzo and conf.autoTuningRate==0 and wave_idx==0){

                std::pair<double,double> results=CompressTest<T,N>(conf, sampled_blocks,QoZ::ALGO_LORENZO_REG,QoZ::TUNING_TARGET_CR,false);
                best_lorenzo_ratio=sizeof(T)*8.0/results.first;
                
                if(conf.verbose)
                    std::cout << "lorenzo best cr = " << best_lorenzo_ratio << std::endl;

            }
               
            if (conf.exhaustiveTuning==0 and conf.autoTuningRate>0){

                if(conf.pdTuningAbConf<=2){               
                    std::pair<double,double> ab=setABwithRelBound(rel_bound,conf.pdTuningAbConf);
                    conf.alpha=ab.first;
                    conf.beta=ab.second;
                }               
                else{
                    conf.alpha=conf.pdAlpha;
                    conf.beta=conf.pdBeta;
                }
                //conf.wavelet_rel_coeff=0.75;
     
            }
          
            std::vector<int> interpAlgo_Candidates={QoZ::INTERP_ALGO_LINEAR, QoZ::INTERP_ALGO_CUBIC};
            std::vector<int> interpDirection_Candidates={0, QoZ::factorial(N) -1};
            if(conf.multiDimInterp)
                interpDirection_Candidates.push_back(QoZ::factorial(N));
            //std::cout<<"pd1"<<std::endl;
            if(conf.levelwisePredictionSelection>0){
                std::vector<uint8_t> interpAlgo_list(conf.levelwisePredictionSelection,0);
                std::vector<uint8_t> interpDirection_list(conf.levelwisePredictionSelection,0);
                auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                                        QoZ::LinearQuantizer<T>(conf.absErrorBound),
                                        QoZ::HuffmanEncoder<int>(),
                                        QoZ::Lossless_zstd());   
                //std::cout<<"pd2"<<std::endl;             
                for(int level=conf.levelwisePredictionSelection;level>0;level--){
                    //std::cout<<"pd3"<<std::endl;
                    int start_level=(level==conf.levelwisePredictionSelection?9999:level);
                    int end_level=level-1;
                    uint8_t bestInterpAlgo = QoZ::INTERP_ALGO_CUBIC;
                    uint8_t bestDirection = 0;
                    double best_interp_absloss=std::numeric_limits<double>::max();
                    //conf.cmprAlgo = QoZ::ALGO_INTERP;                  
                    for (auto &interp_op: interpAlgo_Candidates) {
                        for (auto &interp_direction: interpDirection_Candidates) {
                            //std::cout<<"pd4"<<std::endl;
                            /*
                            if (interp_direction==2 and level<=2)
                                continue;
                            */
                            conf.interpAlgo=interp_op;
                            conf.interpDirection=interp_direction;
                            double cur_absloss=0;
                            for (int i=0;i<num_sampled_blocks;i++){
                                cur_block=sampled_blocks[i];                
                                size_t outSize=0;                              
                                auto cmprData =sz.compress(conf, cur_block.data(), outSize,2,start_level,end_level);
                                delete []cmprData;                              
                                cur_absloss+=conf.decomp_square_error;
                            }
                            //std::cout<<"pd5"<<std::endl;
                            if (cur_absloss<best_interp_absloss){
                                best_interp_absloss=cur_absloss;
                                bestInterpAlgo=interp_op;
                                bestDirection=interp_direction;
                            }
                        }
                    }   
                    interpAlgo_list[level-1]=bestInterpAlgo;
                    interpDirection_list[level-1]=bestDirection;
                    if(conf.pdTuningRealComp){
                        //place to add real compression,need to deal the problem that the sampled_blocks are changed.                   
                        conf.interpAlgo=bestInterpAlgo;
                        conf.interpDirection=bestDirection;
                        for (int i=0;i<num_sampled_blocks;i++){

                            size_t outSize=0;
                                       
                            auto cmprData =sz.compress(conf, sampled_blocks[i].data(), outSize,2,start_level,end_level);
                            delete []cmprData;
                        }
                        
                    }    

                }
                                   
                //conf.interpAlgo_list=interpAlgo_list;
                //conf.interpDirection_list=interpDirection_list;
                interpAlgo_lists[wave_idx]=interpAlgo_list;
                interpDirection_lists[wave_idx]=interpDirection_list;
                if(conf.pdTuningRealComp and conf.autoTuningRate>0 and conf.autoTuningRate==conf.predictorTuningRate){
                        //recover sample if real compression used                  
                    sampleBlocks<T,N>(data,global_dims,sampleBlockSize,sampled_blocks,conf.predictorTuningRate,conf.profiling,starts,conf.var_first);
                }
                    
                if(conf.autoTuningRate==0){              
                    std::pair<double,double> results=CompressTest<T,N>(conf,sampled_blocks,QoZ::ALGO_INTERP,QoZ::TUNING_TARGET_CR,false);
                    double cur_best_interp_cr=sizeof(T)*8.0/results.first;     
                    /*
                    if(wave_idx>1){
                        cur_best_interp_cr*=double(per_block_ele_num)/conf.num;//maybe incorrect;
                    }
                    */
                    if(cur_best_interp_cr>best_interp_cr){
                        best_interp_cr=cur_best_interp_cr;
                        conf.interpAlgo_list=interpAlgo_list;
                        conf.interpDirection_list=interpDirection_list;
                        bestWave=wave_idx;

                    }
                        //if (anchor_rate>0)
                        //  best_interp_cr=1/((1-anchor_rate)/best_interp_cr+anchor_rate);   
                }
                //std::cout<<"pd6"<<std::endl;
            }

            else{
                uint8_t bestInterpAlgo = QoZ::INTERP_ALGO_CUBIC;
                uint8_t bestDirection = 0;
                

                    
                    //conf.cmprAlgo == QoZ::ALGO_INTERP;
                double cur_best_interp_cr=0.0;
                for (auto &interp_op: interpAlgo_Candidates) {
                    for (auto &interp_direction: interpDirection_Candidates) {
                        conf.interpAlgo=interp_op;
                        conf.interpDirection=interp_direction;
                        double cur_ratio=0;
         
                        //block_q_bins.reverse(num_sampled_blocks);
                            
                        std::pair<double,double> results=CompressTest<T,N>(conf, sampled_blocks,QoZ::ALGO_INTERP,QoZ::TUNING_TARGET_CR,false);
                        cur_ratio=sizeof(T)*8.0/results.first;
                        
                        if (cur_ratio>cur_best_interp_cr){
                            cur_best_interp_cr=cur_ratio;
                            bestInterpAlgo=interp_op;
                            bestDirection=interp_direction;
                        }
                    }
                }
                //delete sz;
                bestInterpAlgos[wave_idx]=bestInterpAlgo;
                bestInterpDirections[wave_idx]=bestDirection;
                if(conf.autoTuningRate==0){
                    if(cur_best_interp_cr>best_interp_cr){
                        conf.interpAlgo=bestInterpAlgo;
                        conf.interpDirection=bestDirection;
                        bestWave=wave_idx;
                    }
                }
            }
            conf.absErrorBound=ori_eb;
            if(wave_idx>=1)
                sampled_blocks=ori_sampled_blocks;


        }

        if(conf.verbose)           
            printf("Predictor tuning finished.\n");           
        conf.alpha=o_alpha;
        conf.beta=o_beta;
        conf.dims=global_dims;
        conf.num=global_num;
        //conf.sperr=ori_sperr;
        useInterp= (best_interp_cr>=best_lorenzo_ratio) or best_lorenzo_ratio>=80 or best_interp_cr>=80;//orig 0.95*lorenzo_ratio
        if(conf.verbose and conf.waveletAutoTuning==0){
            if (conf.levelwisePredictionSelection<=0){
                std::cout << "interp best interpAlgo = " << (bestInterpAlgos[0] == 0 ? "LINEAR" : "CUBIC") << std::endl;
                std::cout << "interp best direction = " << (unsigned) bestInterpDirections[0] << std::endl;
                    
            }
            else{
                for(int level=conf.levelwisePredictionSelection;level>0;level--){
                    std::cout << "Level: " << (unsigned) level<<std::endl;
                    std::cout << "\tinterp best interpAlgo = " << (interpAlgo_lists[0][level-1] == 0 ? "LINEAR" : "CUBIC") << std::endl;
                    std::cout << "\tinterp best direction = " << (unsigned) interpDirection_lists[0][level-1] << std::endl;
                }
            }
            if(conf.autoTuningRate==0){
                std::cout << "interp best cr = " << best_interp_cr << std::endl;
                printf("choose %s\n", useInterp ? "interp" : "Lorenzo");
            }
        }

    }
    else{
        QoZ::Timer timer(true);
        //size_t sampling_num, sampling_block;
        //std::vector<size_t> sample_dims(N);         
        sampling_data = QoZ::sampling<T, N>(data, conf.dims, sampling_num, sample_dims, sampling_block);
        QoZ::Config lorenzo_config = conf;
        lorenzo_config.cmprAlgo = QoZ::ALGO_LORENZO_REG;
        lorenzo_config.setDims(sample_dims.begin(), sample_dims.end());
        lorenzo_config.lorenzo = true;
        lorenzo_config.lorenzo2 = true;
        lorenzo_config.regression = false;
        lorenzo_config.regression2 = false;
        lorenzo_config.openmp = false;
        lorenzo_config.blockSize = 5;//why?
        lorenzo_config.quantbinCnt = 65536 * 2;
        //QoZ::writeTextFile<T>("sampled_data.dat", sampling_data.data(), lorenzo_config.num);
            
        size_t sampleOutSize;
        std::vector<T> cur_sampling_data=sampling_data;
        auto cmprData = SZ_compress_LorenzoReg<T, N>(lorenzo_config, cur_sampling_data.data(), sampleOutSize);
            
        delete[]cmprData;
        double ratio = sampling_num * 1.0 * sizeof(T) / sampleOutSize;
        if(conf.verbose)
            printf("Lorenzo ratio = %.4f\n", ratio);

        best_lorenzo_ratio = ratio;
        double best_interp_ratio = 0;


        for (auto &interp_op: {QoZ::INTERP_ALGO_LINEAR, QoZ::INTERP_ALGO_CUBIC}) {
            //cur_sampling_data=sampling_data;
            ratio = do_not_use_this_interp_compress_block_test<T, N>(sampling_data.data(), sample_dims, sampling_num, conf.absErrorBound,
                                                                         interp_op, conf.interpDirection, sampling_block);
            if (ratio > best_interp_ratio) {
                best_interp_ratio = ratio;
                conf.interpAlgo = interp_op;
            }
        }
        if(conf.verbose)
            std::cout << "interp best interpAlgo = " << (conf.interpAlgo == 0 ? "LINEAR" : "CUBIC") << std::endl;
            
        int direction_op = QoZ::factorial(N) - 1;
        //cur_sampling_data=sampling_data;
        ratio = do_not_use_this_interp_compress_block_test<T, N>(sampling_data.data(), sample_dims, sampling_num, conf.absErrorBound,
                                                                     conf.interpAlgo, direction_op, sampling_block);
        if (ratio > best_interp_ratio * 1.02) {
            best_interp_ratio = ratio;
            conf.interpDirection = direction_op;
        }
        useInterp=!(best_lorenzo_ratio > best_interp_ratio && best_lorenzo_ratio < 80 && best_interp_ratio < 80);
        if(conf.verbose){
            std::cout << "interp best direction = " << (unsigned) conf.interpDirection << std::endl;
            
            printf("Interp ratio = %.4f\n", best_interp_ratio);
                
            printf("choose %s\n", useInterp ? "interp" : "Lorenzo");
        }
        if (useInterp){
            conf.cmprAlgo=QoZ::ALGO_INTERP;
        }
        else{
            conf.cmprAlgo=QoZ::ALGO_LORENZO_REG;
        }
        if(conf.verbose)
            timer.stop("sz3 tuning");
    }

    if (useInterp and conf.autoTuningRate>0){
            
        if(conf.verbose)
            std::cout<<"B-M tuning started."<<std::endl;
       
        if (conf.autoTuningRate!=conf.predictorTuningRate and (conf.predictorTuningRate!=0 or conf.autoTuningRate!=conf.waveletTuningRate)){
              
            sampleBlocks<T,N>(data,conf.dims,sampleBlockSize,sampled_blocks,conf.autoTuningRate,0,starts);
        }

        
        double bestalpha=1;
        double bestbeta=1;
        double bestgamma=1;
        double bestb=9999;

        double bestm=0;
        size_t num_sampled_blocks=sampled_blocks.size();
        //std::cout<<num_sampled_blocks<<std::endl;
        size_t per_block_ele_num=pow(sampleBlockSize+1,N);
        size_t ele_num=num_sampled_blocks*per_block_ele_num;
        //vector<double> orig_sums(num_sampled_blocks,0);
        //vector<double> orig_square_sums(num_sampled_blocks,0);
        std::vector<double> orig_means;//(num_sampled_blocks,0);
        std::vector<double> orig_sigma2s;//(num_sampled_blocks,0);
        std::vector<double> orig_ranges;//(num_sampled_blocks,0);
        conf.dims=std::vector<size_t>(N,sampleBlockSize+1);
        conf.num=per_block_ele_num;
        //size_t ssim_size=0;
        //size_t ssim_block_num=0;
        if(conf.tuningTarget==QoZ::TUNING_TARGET_SSIM){
            size_t ssim_size=conf.SSIMBlockSize;
            for (size_t k =0;k<num_sampled_blocks;k++){
                    //cur_block=sampled_blocks[k];
                    //std::cout<<cur_block.size()<<std::endl;
                double orig_mean=0,orig_sigma2=0,orig_range=0;       
                if(N==2){
                    for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                        for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                            std::vector<size_t> starts{i,j};
                            QoZ::blockwise_profiling<T>(sampled_blocks[k].data(),conf.dims,starts,ssim_size,orig_mean,orig_sigma2,orig_range);
                            orig_means.push_back(orig_mean);
                            orig_sigma2s.push_back(orig_sigma2);
                            orig_ranges.push_back(orig_range);


                        }
                    }
                }
                else if(N==3){
                    for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                        for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                            for (size_t kk=0;kk+ssim_size<sampleBlockSize+1;kk+=ssim_size){
                                std::vector<size_t> starts{i,j,kk};
                                QoZ::blockwise_profiling<T>(sampled_blocks[k].data(),conf.dims,starts,ssim_size,orig_mean,orig_sigma2,orig_range);
                                orig_means.push_back(orig_mean);
                                orig_sigma2s.push_back(orig_sigma2);
                                orig_ranges.push_back(orig_range);
                            }
                        }
                    }
                }                      
            }
           //ssim_block_num=orig_means.size();
        }
        std::vector<T> flattened_sampled_data;
           
        if (conf.tuningTarget==QoZ::TUNING_TARGET_AC){

            for(int i=0;i<num_sampled_blocks;i++)
                flattened_sampled_data.insert(flattened_sampled_data.end(),sampled_blocks[i].begin(),sampled_blocks[i].end());

        }
        double oriabseb=conf.absErrorBound;
        for(size_t wave_idx=0;wave_idx<=conf.waveletAutoTuning;wave_idx++){

            if(conf.fixWave>=1 and wave_idx!=conf.fixWave)
                continue;
        //std::vector<double> flattened_cur_blocks;
            std::cout<<wave_idx<<std::endl;
            conf.wavelet=wave_idx;

            if(conf.levelwisePredictionSelection>0){
                conf.interpAlgo_list=interpAlgo_lists[wave_idx];
                conf.interpDirection_list=interpDirection_lists[wave_idx];
            }
            else{
                conf.interpAlgo=bestInterpAlgos[wave_idx];
                conf.interpDirection=bestInterpDirections[wave_idx];
            }
            double sum = std::accumulate(sampled_blocks[4].begin(), sampled_blocks[4].end(), 0.0);
            std::cout<<sum/per_block_ele_num<<std::endl;
            std::vector <std::vector<T> > waveleted_input(sampled_blocks.size());
            if (wave_idx>0 and (wave_idx>1 or !use_sperr<T,N>(conf)) ){
                
                for(size_t i=0;i<sampled_blocks.size();i++){
                    waveleted_input[i].resize(per_block_ele_num);
                    std::copy(sampled_blocks[i].begin(),sampled_blocks[i].end(),waveleted_input[i].begin());
                }
                std::cout<<"t1"<<std::endl;
                double sum = std::accumulate(waveleted_input[4].begin(), waveleted_input[4].end(), 0.0);
                std::cout<<sum/per_block_ele_num<<std::endl;
                if(conf.conditioning){
                    conf.block_metas.clear();
                    conf.block_metas.resize(waveleted_input.size());
                    std::cout<<conf.block_metas.size()<<std::endl;
                    std::cout<<"t1.3"<<std::endl;
                    for(size_t i=0;i<waveleted_input.size();i++){
                        conf.block_metas[i]=pre_Condition<T,N>(conf,waveleted_input[i].data());
                        std::cout<<"t1.6"<<std::endl;

                    }
                    
                }
                std::cout<<"t2"<<std::endl;
                if(wave_idx==1){
                    for(size_t i=0;i<waveleted_input.size();i++){
                        QoZ::Wavelet<T,N> wlt;
                        wlt.preProcess_cdf97(waveleted_input[i].data(),conf.dims);
                    }
                }
                else{
                    std::vector<size_t> coeffs_size;
                    size_t coeffs_num=1;
                    for(size_t i=0;i<waveleted_input.size();i++){
                        //std::cout<<waveleted_input[i].size();
                        //std::cout<<conf.num<<std::endl;
                        T * coeffData=QoZ::external_wavelet_preprocessing<T,N>(waveleted_input[i].data(), conf.dims, conf.num, wave_idx, conf.pid,false,coeffs_size);
                        if(i==0){     
                            //std::cout<<coeffs_size[0]<<std::endl;
                            for (size_t j=0;j<N;j++)
                                coeffs_num*=coeffs_size[j];
                                
                        }
                        std::cout<<"t3"<<std::endl;
                        std::cout<<coeffs_num<<std::endl;

                        waveleted_input[i].clear();
                        waveleted_input[i].resize(coeffs_num);
                        for (size_t j=0;j<coeffs_num;j++)
                            waveleted_input[i][j]=coeffData[j]; 
                        /*
                        std::string oname="waveleted_block_"+std::to_string(i)+"_"+std::to_string(wave_idx)+".tmp";
                        QoZ::writefile<T>(oname.c_str(), waveleted_input[i].data(), coeffs_num);
                        */
                        
                        delete[]coeffData;
                    }
                    std::cout<<"t4"<<std::endl;
                    conf.setDims(coeffs_size.begin(),coeffs_size.end());

                }

            }
            std::vector<double>alpha_list;
            init_alphalist<T,N>(alpha_list,rel_bound,conf);
            size_t alpha_nums=alpha_list.size();
            std::vector<double>beta_list;
            init_betalist<T,N>(beta_list,rel_bound,conf);
            size_t beta_nums=beta_list.size();  
            std::vector<double>gamma_list;
            init_gammalist<T,N>(gamma_list,rel_bound,conf);
            size_t gamma_nums=gamma_list.size();  
            std::cout<<"t5"<<std::endl;
            for(size_t gamma_idx=0;gamma_idx<gamma_nums;gamma_idx++){
                for (size_t i=0;i<alpha_nums;i++){
                    for (size_t j=0;j<beta_nums;j++){
                        conf.absErrorBound=oriabseb;
                        double alpha=alpha_list[i];
                        double beta=beta_list[j];
                        double gamma=gamma_list[gamma_idx];
                        if (( (alpha>=1 and alpha>beta) or (alpha<0 and beta!=-1) ) and !use_sperr<T,N>(conf) )
                            continue;
                        conf.alpha=alpha;
                        conf.beta=beta; 
                        //if(use_sperr<T,N>(conf))
                         //   conf.sperr_eb_coeff=alpha;
                        conf.wavelet_rel_coeff=gamma;
                        if(wave_idx>0 and !use_sperr<T,N>(conf))
                            conf.absErrorBound*=conf.wavelet_rel_coeff;
                        //std::cout<<conf.absErrorBound<<s::std::endl;
                        //std::cout<<"fuqindejian0.1"<<std::endl;                                      
                        std::pair<double,double> results=CompressTest<T,N>(conf, sampled_blocks,QoZ::ALGO_INTERP,(QoZ::TUNING_TARGET)conf.tuningTarget,false,profiling_coeff,orig_means,
                                                                            orig_sigma2s,orig_ranges,flattened_sampled_data,waveleted_input);
                        std::cout<<"t6"<<std::endl;
                        //std::cout<<"fuqindejian0.2"<<std::endl;  
                        double bitrate=results.first;
                        double metric=results.second;
                        printf("%d %.2f %.2f %.2f %.4f %.2f\n",wave_idx,gamma,alpha,beta,bitrate,metric);
                        if ( (conf.tuningTarget!=QoZ::TUNING_TARGET_CR and metric>=bestm and bitrate<=bestb) or (conf.tuningTarget==QoZ::TUNING_TARGET_CR and bitrate<=bestb ) ){
                            bestalpha=alpha;
                            bestbeta=beta;
                            bestgamma=gamma;
                            bestb=bitrate;
                            bestm=metric;
                            bestWave=wave_idx;
                            useInterp=true;
                            printf("Best: %.2f %.2f %.2f %.4f %.2f\n",bestgamma,bestalpha,bestbeta,bestb,bestm);
                        }
                        else if ( (conf.tuningTarget!=QoZ::TUNING_TARGET_CR and metric<=bestm and bitrate>=bestb) or (conf.tuningTarget==QoZ::TUNING_TARGET_CR and bitrate>bestb) ){
                            if ( ((alpha>=1 and pow(alpha,max_interp_level-1)<=beta) or (alpha<1 and alpha*(max_interp_level-1)<=beta)) and !use_sperr<T,N>(conf))
                                break;

                            continue;
                        }
                        else{
                            double eb_fixrate;
                            /*
                            if (metric>bestm)
                                eb_fixrate=rel_bound>1e-4?1.2:1.1;
                            else
                                eb_fixrate=rel_bound>1e-4?0.8:0.9;
                                */
                            eb_fixrate=bitrate/bestb;
                            double orieb=conf.absErrorBound;
                            conf.absErrorBound*=eb_fixrate;
                                
                            std::pair<double,double> results=CompressTest<T,N>(conf, sampled_blocks,QoZ::ALGO_INTERP,(QoZ::TUNING_TARGET)conf.tuningTarget,false,profiling_coeff,orig_means,
                                                                                orig_sigma2s,orig_ranges,flattened_sampled_data,waveleted_input);
                            conf.absErrorBound=orieb;

                            double bitrate_r=results.first;
                            double metric_r=results.second;
                            double a=(metric-metric_r)/(bitrate-bitrate_r);
                            double b=metric-a*bitrate;
                            double reg=a*bestb+b;
                                printf("%.2f %.2f %.2f %.4f %.2f\n",gamma,alpha,beta,bitrate_r,metric_r);
                                printf("%.2f %.2f %.2f %.4f %.2f\n",gamma,alpha,beta,bestb,reg);      
                                //conf.absErrorBound=orig_eb;
                            if (reg>bestm){
                                bestalpha=alpha;
                                bestbeta=beta;
                                bestgamma=gamma;           
                                bestb=bitrate;
                                bestm=metric;
                                bestWave=wave_idx;
                                useInterp=true;
                                    printf("Best: %.2f %.2f %.2f %.4f %.2f\n",bestgamma,bestalpha,bestbeta,bestb,bestm);
                            }
                        }
                        if ( ( (alpha>=1 and pow(alpha,max_interp_level-1)<=beta) or (alpha<1 and alpha*(max_interp_level-1)<=beta)) and !use_sperr<T,N>(conf) )
                            break;

                    }
                }
                std::cout<<"t7"<<std::endl;
                double sum = std::accumulate(sampled_blocks[4].begin(), sampled_blocks[4].end(), 0.0);
                std::cout<<sum/per_block_ele_num<<std::endl;
            }
               // delete sz;
            //add lorenzo
            conf.absErrorBound=oriabseb;
            if(conf.testLorenzo and conf.wavelet==0 and !use_sperr<T,N>(conf)){    


                std::pair<double,double> results=CompressTest<T,N>(conf, sampled_blocks,QoZ::ALGO_LORENZO_REG,(QoZ::TUNING_TARGET)conf.tuningTarget,false,profiling_coeff,orig_means,
                        orig_sigma2s,orig_ranges,flattened_sampled_data,waveleted_input);

                double bitrate=results.first;
                double metric=results.second;

                  
                printf("Lorenzo: %.4f %.2f\n",bitrate,metric);     
                if ( (conf.tuningTarget!=QoZ::TUNING_TARGET_CR and metric>=bestm and bitrate<=bestb) or (conf.tuningTarget==QoZ::TUNING_TARGET_CR and bitrate<=bestb ) ){
                    
                    bestb=bitrate;
                    bestm=metric;
                    bestalpha=-1;
                    bestbeta=-1;
                    bestWave=wave_idx;
                    useInterp=false;
                    printf("Best: %.4f %.2f\n",bestb,bestm);
                       
                }
                else if ( (conf.tuningTarget!=QoZ::TUNING_TARGET_CR and metric<=bestm and bitrate>=bestb) or (conf.tuningTarget==QoZ::TUNING_TARGET_CR and bitrate>bestb) ){
                    useInterp=true;
                }
                else{
                    double eb_fixrate;
                    /*
                    if (metric>bestm)
                        eb_fixrate=rel_bound>1e-4?1.2:1.1;
                    else
                        eb_fixrate=rel_bound>1e-4?0.8:0.9;
                        */
                    eb_fixrate=bitrate/bestb;
                    double orieb=conf.absErrorBound;
                    conf.absErrorBound*=eb_fixrate;                        
                    std::pair<double,double> results=CompressTest<T,N>(conf, sampled_blocks,QoZ::ALGO_LORENZO_REG,(QoZ::TUNING_TARGET)conf.tuningTarget,false,profiling_coeff,orig_means,
                                                                        orig_sigma2s,orig_ranges,flattened_sampled_data,waveleted_input);
                    conf.absErrorBound=orieb;
                    double bitrate_r=results.first;
                    double metric_r=results.second;
                    double a=(metric-metric_r)/(bitrate-bitrate_r);
                    if(a<=0)
                        continue;
                    double b=metric-a*bitrate;
                    double reg=a*bestb+b;
                            printf("%.4f %.2f\n",bitrate_r,metric_r);
                           printf("%.4f %.2f\n",bestb,reg);
                            //conf.absErrorBound=orig_eb;
                    if (reg>bestm){
                               // bestalpha=alpha;
                                //bestbeta=beta; 
                        bestb=bitrate;
                        bestm=metric;
                        bestalpha=-1;
                        bestbeta=-1;
                        bestWave=wave_idx;
                        useInterp=false;

                                printf("Best: %.4f %.2f\n",bestb,bestm);
                    }
                }          
            }
            conf.absErrorBound=oriabseb;
            
        }
        if(conf.tuningTarget==QoZ::TUNING_TARGET_AC){
            bestm=1-bestm;
        }
        std::string metric_name="no";
        if (conf.tuningTarget==QoZ::TUNING_TARGET_RD ){
            metric_name="PSNR";
        }
        else if (conf.tuningTarget==QoZ::TUNING_TARGET_SSIM ){
            metric_name="SSIM";
        }
        else if (conf.tuningTarget==QoZ::TUNING_TARGET_AC ){
            metric_name="AutoCorrelation";
        }
        if(conf.verbose){
            printf("Autotuning finished.\n");
            printf("Selected wavelet: %d\n",bestWave);
            if (useInterp)
                printf("Interp selected. Selected gamma: %f. Selected alpha: %f. Selected beta: %f. Best bitrate: %f. Best %s: %f.\n",bestgamma,bestalpha,bestbeta,bestb, const_cast<char*>(metric_name.c_str()),bestm);
            else
                printf("Lorenzo selected. Best bitrate: %f. Best %s: %f.\n",bestb, const_cast<char*>(metric_name.c_str()),bestm);

        }
        conf.alpha=bestalpha;
        conf.beta=bestbeta;
        conf.wavelet_rel_coeff=bestgamma;
        conf.dims=global_dims;
        conf.num=global_num;  
        conf.wavelet=bestWave;
        //if(use_sperr<T,N>(conf))
        //   conf.sperr_eb_coeff=bestalpha;
        if(useInterp){ 

            if(conf.levelwisePredictionSelection>0){
                conf.interpAlgo_list=interpAlgo_lists[bestWave];
                conf.interpDirection_list=interpDirection_lists[bestWave];
            }
            else{
                conf.interpAlgo=bestInterpAlgos[bestWave];
                conf.interpDirection=bestInterpDirections[bestWave];
            }
        }
    }

    if (useInterp){
        conf.cmprAlgo=QoZ::ALGO_INTERP;
    }
    else{
         conf.cmprAlgo=QoZ::ALGO_LORENZO_REG;
    } 
        
    for(int i=0;i<sampled_blocks.size();i++){
        std::vector< T >().swap(sampled_blocks[i]);              
    }
    std::vector< std::vector<T> >().swap(sampled_blocks);     
    return best_lorenzo_ratio;    
}



template<class T, QoZ::uint N>
char *SZ_compress_Interp_lorenzo(QoZ::Config &conf, T *data, size_t &outSize) {
    assert(conf.cmprAlgo == QoZ::ALGO_INTERP_LORENZO);
    QoZ::calAbsErrorBound(conf, data);
    double prewave_absErrorBound=conf.absErrorBound;
    
    T *origdata,*coeffData;
    if (conf.rng<0)
        conf.rng=QoZ::data_range<T>(data,conf.num);
    
    if (conf.relErrorBound<=0)
        conf.relErrorBound=conf.absErrorBound/conf.rng;
   // T* coeffs;
    bool useSperr=use_sperr<T,N>(conf);
    if(useSperr and conf.waveletAutoTuning==0 and conf.wavelet<=1){
        conf.cmprAlgo = QoZ::ALGO_INTERP;
        

        //std::cout<<"s5"<<std::endl;
        return SPERR_Compress<T,N>(conf,data,outSize);
        //rtn = sperr::write_n_bytes(output_file, stream.size(), stream.data());

      

    }
    //conf.sperr=0;
    std::vector<size_t> coeffs_size;
    std::vector<size_t> orig_dims=conf.dims;
    size_t orig_num=conf.num;
    int ori_wave=0;
    if(conf.wavelet>0 and conf.waveletAutoTuning==0){       


        if(conf.conditioning and (useSperr or conf.wavelet>1)  ){
            auto meta=pre_Condition<T,N>(conf,data);
            conf.meta=meta;
        }
        
        
        if(conf.wavelet>1){
            //read a coeff array and a size information array
            coeffData=QoZ::external_wavelet_preprocessing<T,N>(data, conf.dims, conf.num, conf.wavelet, conf.pid, false, coeffs_size);
            conf.setDims(coeffs_size.begin(),coeffs_size.end());
            
        }
        else{
  
           // std::cout<<"origdatanew"<<std::endl;
            origdata=new T[conf.num];
            memcpy(origdata,data,conf.num*sizeof(T));

            QoZ::Wavelet<T,N> wlt;
            wlt.preProcess_cdf97(data,conf.dims);
            //conf.errorBoundMode = QoZ::EB_REL;
            //conf.relErrorBound/=conf.wavelet_rel_coeff;
            //QoZ::calAbsErrorBound(conf, data);
        }
        if(!useSperr)
            conf.absErrorBound*=conf.wavelet_rel_coeff;
        if(conf.coeffTracking%2==1)
            QoZ::writefile<T>("waved.qoz.ori.dwt", data, conf.num);
        if(conf.coeffTracking>1){
            size_t count=0;
            if(conf.wavelet>1){
                for (size_t i=0;i<conf.num;i++){
                    if(fabs(coeffData[i])>conf.absErrorBound)
                        count++;
                }

            }
            else{
                for (size_t i=0;i<conf.num;i++){
                    if(fabs(data[i])>conf.absErrorBound)
                        count++;
                }

            }
            std::cout<<"Significant coeff rate: "<<(float)count/conf.num<<std::endl;
        }
        ori_wave=conf.wavelet;
        
            
            //std::cout<<conf.transformation<<std::endl;
            /*
            if(conf.transformation==1){
                for(size_t i=0;i<conf.num;i++)
                    data[i]=QoZ::sigmoid<double>(data[i]);
                //std::cout<<"transed"<<std::endl;
            }
            else if(conf.transformation==2){
                for(size_t i=0;i<conf.num;i++)
                    data[i]=QoZ::tanh<double>(data[i]);
            } 
            */
            //QoZ::writefile<T>("waved.qoz.ori.sigmo", data, conf.num);    
    }
   
    conf.wavelet=0; 


    if(conf.preTrim>0){
        if(ori_wave>1){
            for(size_t i=0;i<conf.num;i++){
                if(fabs(coeffData[i])<=conf.preTrim*conf.absErrorBound)
                    coeffData[i]=0;
            }
        }
        else{
            for(size_t i=0;i<conf.num;i++){
                if(fabs(data[i])<=conf.preTrim*conf.absErrorBound)
                    data[i]=0;
            }
        }
    }

    if(conf.verbose)
        std::cout << "====================================== BEGIN TUNING ================================" << std::endl;
    QoZ::Timer timer(true);
    double best_lorenzo_ratio=1.0;
    if(ori_wave>1){   
        if(!useSperr)
            best_lorenzo_ratio=Tuning<T,N>(conf,coeffData);
        else
            conf.cmprAlgo = QoZ::ALGO_INTERP;
    }
    else{    
        best_lorenzo_ratio=Tuning<T,N>(conf,data);
    }
    char * compress_output;

    if(conf.waveletAutoTuning>0 and conf.wavelet>0){
        //std::cout<<"wavelet actively selected."<<std::endl;

        if(conf.conditioning and (!use_sperr<T,N>(conf) or conf.wavelet>1)){
            auto meta=pre_Condition<T,N>(conf,data);
            conf.meta=meta;
        }

        if(conf.wavelet==1){

            if (use_sperr<T,N>(conf)){
                conf.cmprAlgo = QoZ::ALGO_INTERP;
                double tuning_time = timer.stop();
                if(conf.verbose){
                    std::cout << "Tuning time = " << tuning_time << "s" << std::endl;
                    std::cout << "====================================== END TUNING ======================================" << std::endl;
                }
                return SPERR_Compress<T,N>(conf,data,outSize);
            }


            conf.absErrorBound*=conf.wavelet_rel_coeff;
            origdata=new T[conf.num];
            memcpy(origdata,data,conf.num*sizeof(T));
            QoZ::Wavelet<T,N> wlt;
            wlt.preProcess_cdf97(data,conf.dims);//temp
            if(conf.coeffTracking%2==1)
                QoZ::writefile<T>("waved.qoz.ori.dwt", data, conf.num);
        }
        else{
            coeffData=QoZ::external_wavelet_preprocessing<T,N>(data, conf.dims, conf.num, conf.wavelet, conf.pid, false, coeffs_size);
            conf.setDims(coeffs_size.begin(),coeffs_size.end());


        }
           
    }
    else if (conf.waveletAutoTuning==0){
        conf.wavelet=ori_wave;
    }

    
    //conf.cmprAlgo =QoZ::ALGO_INTERP; 
    /*
    std::vector<T> orig_data(conf.num,0);
    for(int i=0;i<conf.num;i++)
        orig_data[i]=data[i];
    */
    //}
//    printf("%lu %lu %lu %lu %lu\n", sampling_data.size(), sampling_num, sample_dims[0], sample_dims[1], sample_dims[2]);
   // bool useInterp = !(best_lorenzo_ratio > best_interp_ratio && best_lorenzo_ratio < 80 && best_interp_ratio < 80);
    
//    printf("\nLorenzo compression ratio = %.2f\n", best_lorenzo_ratio);
//    printf("Interp compression ratio = %.2f\n", best_interp_ratio);  

    if (conf.cmprAlgo == QoZ::ALGO_INTERP) {
         //std::cout << "pos8 "<< std::endl;     
        std::vector<int>().swap(conf.quant_bins);
        double tuning_time = timer.stop();
        if(conf.verbose){
            std::cout << "Tuning time = " << tuning_time << "s" << std::endl;
            std::cout << "====================================== END TUNING ======================================" << std::endl;
        }
        /*
        for(int i=0;i<sampled_blocks.size();i++){
            std::vector< T >().swap(sampled_blocks[i]);
           
        }
        std::vector< std::vector<T> >().swap(sampled_blocks);
        */

       /*
        std::cout<<conf.alpha<<std::endl;
        std::cout<<conf.beta<<std::endl;
        if (conf.levelwisePredictionSelection<=1){
            std::cout<<int(conf.interpAlgo)<<std::endl;
            std::cout<<int(conf.interpDirection)<<std::endl;

        }
        else{
            for(int i=conf.levelwisePredictionSelection;i>0;i-- ){
                std::cout<<int(conf.interpAlgo_list[i-1])<<std::endl;
                std::cout<<int(conf.interpDirection_list[i-1])<<std::endl;
            }
        }
        */
        if (conf.predictorTuningRate<1){      
            if(conf.wavelet >1)
                if(use_sperr<T,N>(conf)){
                    compress_output = SPERR_Compress<T,N>(conf, coeffData, outSize);
                }
                else
                    compress_output = SZ_compress_Interp<T, N>(conf, coeffData, outSize);
            else{
                if(use_sperr<T,N>(conf)){
                    compress_output = SPERR_Compress<T,N>(conf,data, outSize);//This case is not error bounded.

                }
                else
                    compress_output = SZ_compress_Interp<T, N>(conf, data, outSize);        
            }
        }
        else {
            std::vector<int> op_candidates={QoZ::INTERP_ALGO_LINEAR,QoZ::INTERP_ALGO_CUBIC};
            std::vector<int> dir_candidates={0,QoZ::factorial(N)-1};
            if(conf.wavelet >1){
                //std::cout<<"dawda"<<std::endl;
                compress_output = SZ_compress_AutoSelectiveInterp<T,N>(conf,coeffData,outSize,op_candidates,dir_candidates,0);
            }
            else
                compress_output = SZ_compress_AutoSelectiveInterp<T,N>(conf,data,outSize,op_candidates,dir_candidates,0);
        }
    } 

    else {
        QoZ::Config lorenzo_config = conf;
        size_t sampling_num, sampling_block;        
        std::vector<size_t> sample_dims(N);
        std::vector<T> sampling_data;

        size_t sampleOutSize;
        double ratio;  
            //size_t sampling_num, sampling_block;
            
        sampling_data = QoZ::sampling<T, N>(data, conf.dims, sampling_num, sample_dims, sampling_block);        
        lorenzo_config.cmprAlgo = QoZ::ALGO_LORENZO_REG;
        lorenzo_config.setDims(sample_dims.begin(), sample_dims.end());
        lorenzo_config.lorenzo = true;
        lorenzo_config.lorenzo2 = true;
        lorenzo_config.regression = false;
        lorenzo_config.regression2 = false;
        lorenzo_config.openmp = false;
        lorenzo_config.blockSize = 5;//why?
        lorenzo_config.quantbinCnt = 65536 * 2;
                    
        if(conf.autoTuningRate>0 or conf.predictorTuningRate>0){
            auto cmprData = SZ_compress_LorenzoReg<T, N>(lorenzo_config, sampling_data.data(), sampleOutSize);
            delete[]cmprData;
            ratio = sampling_num * 1.0 * sizeof(T) / sampleOutSize;
            //printf("Lorenzo ratio = %.2f\n", ratio);
            best_lorenzo_ratio = ratio;
        }
      
        //further tune lorenzo
        if (N == 3 and !conf.useCoeff) {
            lorenzo_config.quantbinCnt = QoZ::optimize_quant_invl_3d<T>(data, conf.dims[0], conf.dims[1], conf.dims[2], conf.absErrorBound);
            lorenzo_config.pred_dim = 2;
            auto cmprData = SZ_compress_LorenzoReg<T, N>(lorenzo_config, sampling_data.data(), sampleOutSize);
            delete[]cmprData;
            ratio = sampling_num * 1.0 * sizeof(T) / sampleOutSize;
            //printf("Lorenzo, pred_dim=2, ratio = %.4f\n", ratio);
            if (ratio > best_lorenzo_ratio * 1.02) {
                best_lorenzo_ratio = ratio;
            } else {
                lorenzo_config.pred_dim = 3;
            }
        }
        if (conf.relErrorBound < 1.01e-6 && best_lorenzo_ratio > 5) {
            auto quant_num = lorenzo_config.quantbinCnt;
            lorenzo_config.quantbinCnt = 16384;
            auto cmprData = SZ_compress_LorenzoReg<T, N>(lorenzo_config, sampling_data.data(), sampleOutSize);
            delete[]cmprData;
            ratio = sampling_num * 1.0 * sizeof(T) / sampleOutSize;
//            printf("Lorenzo, quant_bin=8192, ratio = %.2f\n", ratio);
            if (ratio > best_lorenzo_ratio * 1.02) {
                best_lorenzo_ratio = ratio;
            } else {
                lorenzo_config.quantbinCnt = quant_num;
            }
        }
     
        lorenzo_config.setDims(conf.dims.begin(), conf.dims.end());
        conf = lorenzo_config;
        /*
        if(conf.useCoeff){
            
            if (conf.lorenzo){
                size_t num_coeff=int(pow(2,N)-1);
                std::vector <double> A;
                std::vector<double> b;
                if(N==2)
                    QoZ::extract_lorenzoreg_2d<T,N>(data, A, b, conf.dims,1,conf.regSampleStep);
                else if (N==3)
                    QoZ::extract_lorenzoreg_3d<T,N>(data, A, b, conf.dims,1,conf.regSampleStep);
                //std::cout<<"step1"<<std::endl;
                size_t num_points=b.size();
                double * coeff_array=QoZ::Regression(A.data(),num_points,num_coeff,b.data());
                //std::cout<<"step2"<<std::endl;
                conf.lorenzo1_coeffs=std::vector<double>(coeff_array,coeff_array+num_coeff);
                delete [] coeff_array;

            }
            if (conf.lorenzo2){
                size_t num_coeff=int(pow(3,N)-1);
                std::vector <double> A;
                std::vector<double> b;
                if(N==2)
                    QoZ::extract_lorenzoreg_2d<T,N>(data, A, b, conf.dims,2,conf.regSampleStep);
                else if (N==3)
                    QoZ::extract_lorenzoreg_3d<T,N>(data, A, b, conf.dims,2,conf.regSampleStep);
                //std::cout<<"step3"<<std::endl;
                size_t num_points=b.size();
                double * coeff_array=QoZ::Regression(A.data(),num_points,num_coeff,b.data());
                //std::cout<<"step4"<<std::endl;
                conf.lorenzo2_coeffs=std::vector<double>(coeff_array,coeff_array+num_coeff);
                delete [] coeff_array;

            }
        }
        */
        //std::cout<<conf.quantbinCnt<<std::endl;
        double tuning_time = timer.stop();
        if(conf.verbose){
            std::cout << "Tuning time = " << tuning_time << "s" << std::endl;
            std::cout << "====================================== END TUNING ======================================" << std::endl;
        }
        compress_output = SZ_compress_LorenzoReg<T, N>(conf, data, outSize);
    }
    //std::cout<<conf.wavelet<<std::endl;

    if(conf.wavelet>0){
        //if(conf.coeffTracking>0)
        std::cout<<"Coeff CR = "<<(orig_num*1.0*sizeof(T))/outSize<<std::endl; 
        conf.firstSize=outSize;
        size_t tempSize=outSize; 
        T * decData;
      //for(size_t i=0;i<N;i++)
           //std::cout<<conf.dims[i]<<std::endl;
        //std::cout<<conf.num<<std::endl;
        //std::cout<<"decdatanew"<<std::endl;





        //T *decData =new T [conf.num];
        //int ori_wave=conf.wavelet;
        //conf.wavelet=0;
        //can skip the following decomp, do later.
        /*
        if(conf.cmprAlgo == QoZ::ALGO_INTERP){
            SZ_decompress_Interp<T,N>(conf,compress_output,tempSize,decData);
        }
        else{
            SZ_decompress_LorenzoReg<T, N>(conf, compress_output, tempSize,decData);

        }
        if(conf.coeffTracking%2==1)
            QoZ::writefile<T>("waved.qoz.cmp.dwt", decData, conf.num);
        */
        //conf.wavelet=ori_wave;




        //std::cout<<"p2"<<std::endl;
        //QoZ::writefile<T>("waved.qoz.cmp.sigmo", decData, conf.num);
        /*

        if(conf.transformation==1){
            for(size_t i=0;i<conf.num;i++)
                decData[i]=QoZ::logit<double>(decData[i]);
        }
        else if(conf.transformation==2){
            for(size_t i=0;i<conf.num;i++)
                decData[i]=QoZ::arctanh<double>(decData[i]);
        } 
        */
        //QoZ::writefile<T>("waved.qoz.cmp.logit", decData, conf.num);



        if(conf.wavelet>1){
            //save data to file
            //run system()
            //read back the decdata
            //std::cout<<"coeffdatadel"<<std::endl;
            //std::cout<<"a1"<<std::endl;
            if(use_sperr<T,N>(conf)){
                //std::cout<<"a2"<<std::endl;
                //std::cout<<conf.num<<std::endl;
                //std::cout<<conf.num<<std::endl;
                coeffData=new T[conf.num];
                SPERR_Decompress<T,N>(compress_output,outSize,coeffData);
            
                
                
            }
           //std::cout<<"a3"<<std::endl;
            decData=QoZ::external_wavelet_postprocessing<T,N>(coeffData, conf.dims, conf.num,conf.wavelet, conf.pid, false,orig_dims);

           // std::cout<<"a4"<<std::endl;
            delete []coeffData;
           // std::cout<<"a5"<<std::endl;
            conf.coeffs_dims=conf.dims;
            conf.coeffs_num=conf.num;
            conf.num=orig_num;
            conf.dims=orig_dims;
            //for(size_t i=0;i<N;i++)
                //std::cout<<conf.dims[i]<<std::endl;
            //std::cout<<conf.num<<std::endl;
            //std::cout<<"decdatadel"<<std::endl;
            
            if(conf.coeffTracking%2==1)
                QoZ::writefile<T>("waved.qoz.cmp.idwt", decData, conf.num);
            if(conf.conditioning){
                auto rtn=post_Condition<T,N>(decData,conf.num,conf.meta);
                rtn=post_Condition<T,N>(data,conf.num,conf.meta);
                if(conf.coeffTracking%2==1)
                    QoZ::writefile<T>("waved.qoz.cmp.afterpost", decData, conf.num);
                
            }
            
            //std::cout<<"p4"<<std::endl;
            for(size_t i=0;i<conf.num;i++){
                decData[i]=data[i]-decData[i];
            }
        }
        else{
            if(use_sperr<T,N>(conf)){
                //std::cout<<"a2"<<std::endl;
                //std::cout<<conf.num<<std::endl;
                //std::cout<<conf.num<<std::endl;
                
                SPERR_Decompress<T,N>(compress_output,outSize,data);
            
                
                
            }
            else{
                QoZ::Wavelet<T,N> wlt;
                wlt.postProcess_cdf97(data,conf.dims);               
            }
            decData=data;//maybe need to fix
            if(conf.coeffTracking%2==1)
                QoZ::writefile<T>("waved.qoz.cmp.idwt", decData, conf.num);
                
            
            if(conf.conditioning and !use_sperr<T,N>(conf)){
                auto rtn=post_Condition<T,N>(decData,conf.num,conf.meta);
                rtn=post_Condition<T,N>(origdata,conf.num,conf.meta);
                if(conf.coeffTracking%2==1)
                    QoZ::writefile<T>("waved.qoz.cmp.afterpost", decData, conf.num);
                
            }
            for(size_t i=0;i<conf.num;i++){
                decData[i]=origdata[i]-decData[i];

            }

            //std::cout<<"origdatadel"<<std::endl;
            delete []origdata;
        }

        /*
        size_t count=0;
        for(size_t i=0;i<conf.num;i++){
            if(fabs(decData[i])>prewave_absErrorBound)
                count++;
        }
          
        std::cout<<count<<std::endl;
        */
        
        //std::cout<<"p5"<<std::endl;
        //QoZ::writefile<T>("waved.qoz.cmp.offset", decData, conf.num);
        QoZ::Config newconf(conf.num);
        newconf.absErrorBound=prewave_absErrorBound;
      
        //newconf.blockSize=32768;
        size_t outlier_outSize=0;
        char * outlier_compress_output;
        outlier_compress_output=outlier_compress<T,N>(newconf,decData,outlier_outSize);
        //std::cout<<outlier_outSize<<std::endl;

        size_t totalsize=outSize+outlier_outSize;
        char * final_output=new char[totalsize+conf.size_est()];
        //for(size_t i=0;i<totalsize;i++)
            //final_output[i]=0;
        memcpy(final_output,compress_output,outSize);
        memcpy(final_output+outSize,outlier_compress_output,outlier_outSize);
        outSize=totalsize;
        //std::cout<<"p7"<<std::endl;
        delete [] compress_output;
        delete [] outlier_compress_output;
        //std::cout<<"decdatadel"<<std::endl;
        if(conf.wavelet>1)//maybe need to fix
            delete [] decData;
        //std::cout<<"p8"<<std::endl;
        return final_output;
    }
    else{
        return compress_output;
    }


}


template<class T, QoZ::uint N>
char *SZ_compress_Interp_blocked(QoZ::Config &conf, T *data, size_t &outSize) {

    assert(conf.cmprAlgo == QoZ::ALGO_INTERP_BLOCKED);  
    conf.cmprAlgo=QoZ::ALGO_INTERP;
    QoZ::Timer timer(true);
    QoZ::calAbsErrorBound(conf, data);
    T rng=QoZ::data_range<T>(data,conf.num);
    double rel_bound=conf.absErrorBound/rng;
    size_t sampling_num, sampling_block;
    double best_lorenzo_ratio=0.0;

    std::vector<T> sampling_data;
    std::vector< std::vector<T> > sampled_blocks;
    std::vector< std::vector<size_t> > starts;
    size_t totalblock_num=1;
    if (conf.interpBlockSize<=0){
            conf.interpBlockSize = (N==2?64:32);
    }
    /*
    if(conf.blockwiseSampleBlockSize<=0){
        conf.blockwiseSampleBlockSize=(N==2?32:16);
    }
    */
    int max_interp_level=(int)log2(conf.interpBlockSize)+1;
    if(conf.maxStep>0){
        int temp=(int)log2(conf.maxStep);
        if (temp<max_interp_level)
            max_interp_level=temp;
    }
    if (conf.levelwisePredictionSelection>max_interp_level)
        conf.levelwisePredictionSelection=max_interp_level;
    size_t sampleBlockSize=conf.sampleBlockSize;
    if (sampleBlockSize<=0)
        sampleBlockSize=conf.interpBlockSize;
    size_t min_sbs=16;
    size_t min_sbsbs=8;
    size_t min_bsbs=4;
    if (sampleBlockSize<min_sbs){
        sampleBlockSize=min_sbs;

    }
    if(conf.sampleBlockSampleBlockSize==0){
        conf.sampleBlockSampleBlockSize=sampleBlockSize;
    }

    if(conf.sampleBlockSampleBlockSize<min_sbsbs){
        conf.sampleBlockSampleBlockSize=min_sbsbs;
    }
    if(conf.blockwiseSampleBlockSize==0){
        conf.blockwiseSampleBlockSize=conf.interpBlockSize;
    }
    if(conf.blockwiseSampleBlockSize<min_bsbs){
        conf.blockwiseSampleBlockSize=min_bsbs;
    }

    //if(conf.blockwiseSampleBlockSize>sampleBlockSize)
       // conf.blockwiseSampleBlockSize=sampleBlockSize;

    conf.fixBlockSize=conf.interpBlockSize;

    size_t num_filtered_blocks=0;
    if(conf.autoTuningRate>0  and conf.profiling){
        if(N==2){
            QoZ::profiling_block_2d<T,N>(data,conf.dims,starts,sampleBlockSize,conf.absErrorBound,conf.profStride);
        }
        else if (N==3){
            QoZ::profiling_block_3d<T,N>(data,conf.dims,starts,sampleBlockSize,conf.absErrorBound,conf.profStride);
        }

    }
    num_filtered_blocks=starts.size();
    std::vector<size_t> global_dims=conf.dims;
    size_t global_num=conf.num;
    /*
    std::vector<T> orig_data(conf.num,0);
    for(int i=0;i<conf.num;i++)
        orig_data[i]=data[i];
    */
//    printf("%lu %lu %lu %lu %lu\n", sampling_data.size(), sampling_num, sample_dims[0], sample_dims[1], sample_dims[2]);
    {
        //tune interp
        if ( conf.autoTuningRate>0 ){
            //std::vector<size_t> global_dims=conf.dims;
            //size_t orig_maxStep=conf.maxStep;
            //conf.maxStep=conf.dims[0]-1;
            //size_t global_num=conf.num;
            totalblock_num=1;
            for(int i=0;i<N;i++){
                totalblock_num*=(int)((conf.dims[i]-1)/sampleBlockSize);
            }
                //sampled_blocks.resize( (int)((totalblock_num-1)/sample_ratio)+1 );
            sampleBlocks<T,N>(data,conf.dims,sampleBlockSize,sampled_blocks,conf.autoTuningRate,conf.profiling,starts);  
            
            double anchor_rate=0;
            if (conf.maxStep>0){
                anchor_rate=1/(pow(conf.maxStep,N));
            }
            //std::cout<<"step 2"<<std::endl;
            std::vector<double>alpha_list;
            init_alphalist<T,N>(alpha_list,rel_bound,conf);
            size_t alpha_nums=alpha_list.size();
            std::vector<double>beta_list;

            init_betalist<T,N>(beta_list,rel_bound,conf);
            size_t beta_nums=beta_list.size();
            double bestalpha=1;
            double bestbeta=1;
            double bestb=9999;
            double bestm=0;
            size_t num_sampled_blocks=sampled_blocks.size();
            size_t per_block_ele_num=pow(sampleBlockSize+1,N);
            size_t ele_num=num_sampled_blocks*per_block_ele_num;
            std::vector<T> cur_block(per_block_ele_num,0);
            std::vector<int> op_candidates={QoZ::INTERP_ALGO_LINEAR,QoZ::INTERP_ALGO_CUBIC};
            std::vector<int> dir_candidates={0,QoZ::factorial(N)-1};
            std::vector<double> orig_means;//(num_sampled_blocks,0);
            std::vector<double> orig_sigma2s;//(num_sampled_blocks,0);
            std::vector<double> orig_ranges;//(num_sampled_blocks,0);
            conf.dims=std::vector<size_t>(N,sampleBlockSize+1);
            conf.num=per_block_ele_num;
            size_t ssim_size=0;
            size_t ssim_block_num=0;

            if(conf.tuningTarget==QoZ::TUNING_TARGET_SSIM){
                ssim_size=conf.SSIMBlockSize;
                for (size_t k =0;k<num_sampled_blocks;k++){
                        //cur_block=sampled_blocks[k];
                        //std::cout<<cur_block.size()<<std::endl;
                        double orig_mean=0,orig_sigma2=0,orig_range=0;
                        
                        if(N==2){
                            for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                                for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                                    std::vector<size_t> starts{i,j};
                                    QoZ::blockwise_profiling<T>(sampled_blocks[k].data(),conf.dims,starts,ssim_size,orig_mean,orig_sigma2,orig_range);
                                    orig_means.push_back(orig_mean);
                                    orig_sigma2s.push_back(orig_sigma2);
                                    orig_ranges.push_back(orig_range);
                                }
                            }
                        }

                        else if(N==3){
                            for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                                for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                                    for (size_t kk=0;kk+ssim_size<sampleBlockSize+1;kk+=ssim_size){
                                        std::vector<size_t> starts{i,j,kk};
                                        QoZ::blockwise_profiling<T>(sampled_blocks[k].data(),conf.dims,starts,ssim_size,orig_mean,orig_sigma2,orig_range);
                                        orig_means.push_back(orig_mean);
                                        orig_sigma2s.push_back(orig_sigma2);
                                        orig_ranges.push_back(orig_range);
                                    }
                                }
                            }
                        }       
                        //std::cout<<"step 3.5"<<std::endl;
                        //std::cout<<conf.quant_bins.size()<<std::endl;
                        //std::cout<<conf.decomp_square_error<<std::endl;
                }
                ssim_block_num=orig_means.size();
            }
            //std::cout<<num_sampled_blocks<<std::endl;
            //std::cout<<per_block_ele_num<<std::endl;
            //std::cout<<ele_num<<std::endl;
            //std::cout<<"step 3"<<std::endl;
            std::vector<T> flattened_sampled_data;      
            if (conf.tuningTarget==QoZ::TUNING_TARGET_AC){
                for(int i=0;i<num_sampled_blocks;i++)
                    flattened_sampled_data.insert(flattened_sampled_data.end(),sampled_blocks[i].begin(),sampled_blocks[i].end());
            }
          
            std::vector<T> flattened_cur_blocks;
            auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
                            QoZ::LinearQuantizer<T>(conf.absErrorBound),
                            QoZ::HuffmanEncoder<int>(),
                            QoZ::Lossless_zstd());
        
            for (size_t i=0;i<alpha_nums;i++){
                for (size_t j=0;j<beta_nums;j++){
                    double alpha=alpha_list[i];
                    double beta=beta_list[j];
                    if ((alpha>=1 and alpha>beta) or (alpha<0 and beta!=-1))
                        continue;  
                    conf.alpha=alpha;
                    conf.beta=beta;
                    std::vector<int> q_bins;
                    std::vector<std::vector<int> > block_q_bins;
                    //block_q_bins.reverse(num_sampled_blocks);
                    std::vector<size_t> q_bin_counts;
                    double square_error=0.0;
                    double metric=0;
                    size_t idx=0;
                    for (size_t k = 0;k<num_sampled_blocks;k++){
                        cur_block=sampled_blocks[k];
                        //std::cout<<cur_block.size()<<std::endl;
                        size_t tempsize;
                        SZ_compress_AutoSelectiveInterp_with_sampling<T,N>(conf,cur_block.data(),tempsize,op_candidates,dir_candidates,conf.sampleBlockSampleBlockSize,1);
                        //SZ_compress_AutoSelectiveInterp<T,N>(conf, cur_block.data(), tempsize,op_candidates,dir_candidates,1);
                        //std::cout<<"step 3.5"<<std::endl;
                        //std::cout<<conf.quant_bins.size()<<std::endl;
                        //std::cout<<conf.decomp_square_error<<std::endl;
                        block_q_bins.push_back(conf.quant_bins);
                        square_error+=conf.decomp_square_error;
                        if (conf.tuningTarget==QoZ::TUNING_TARGET_SSIM){             
                            double mean=0,sigma2=0,cov=0,range=0;
                            double orig_mean=0,orig_sigma2=0,orig_range=0;                    
                            if(N==2){
                                for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                                    for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                                        orig_mean=orig_means[idx];
                                        orig_sigma2=orig_sigma2s[idx];
                                        orig_range=orig_ranges[idx];
                                        std::vector<size_t> starts{i,j};
                                        QoZ::blockwise_profiling<T>(cur_block.data(),conf.dims,starts,ssim_size,mean,sigma2,range);
                                        cov=QoZ::blockwise_cov<T>(sampled_blocks[k].data(),cur_block.data(),conf.dims,starts,ssim_size,orig_mean,mean);
                                        metric+=QoZ::SSIM(orig_range,orig_mean,orig_sigma2,mean,sigma2,cov)/ssim_block_num;
                                        idx++;
                                    }
                                }
                            }
                            else if(N==3){
                                for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                                    for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                                        for (size_t kk=0;kk+ssim_size<sampleBlockSize+1;kk+=ssim_size){
                                            orig_mean=orig_means[idx];
                                            orig_sigma2=orig_sigma2s[idx];
                                            orig_range=orig_ranges[idx];
                                            std::vector<size_t> starts{i,j,kk};
                                            QoZ::blockwise_profiling<T>(cur_block.data(),conf.dims,starts,ssim_size,mean,sigma2,range);
                                            
                                            cov=QoZ::blockwise_cov<T>(sampled_blocks[k].data(),cur_block.data(),conf.dims,starts,ssim_size,orig_mean,mean);
                                            //printf("%.8f %.8f %.8f %.8f %.8f %.8f %.8f\n",orig_range,orig_sigma2,orig_mean,range,sigma2,mean,cov);
                                            metric+=QoZ::SSIM(orig_range,orig_mean,orig_sigma2,mean,sigma2,cov)/ssim_block_num;
                                            idx++;
                                        }
                                    }
                                }
                            }
                        }
                        else if (conf.tuningTarget==QoZ::TUNING_TARGET_AC){
                            flattened_cur_blocks.insert(flattened_cur_blocks.end(),cur_block.begin(),cur_block.end());
                        }

                    }
                    //std::cout<<square_error<<std::endl;
                    q_bin_counts=conf.quant_bin_counts;
                    //std::cout<<"step 4"<<std::endl;
                    size_t level_num=q_bin_counts.size();
                    //std::cout<<level_num<<std::endl;
                    size_t last_pos=0;
                    for(int k=level_num-1;k>=0;k--){
                        for (size_t l =0;l<num_sampled_blocks;l++){
                            //std::cout<<block_q_bins[l].size()<<std::endl;
                            //std::cout<<q_bin_counts[k]<<std::endl;
                            for (size_t m=last_pos;m<q_bin_counts[k];m++){
                                q_bins.push_back(block_q_bins[l][m]);
                            }
                        }   
                        last_pos=q_bin_counts[k];
                       // std::cout<<last_pos<<std::endl;
                    }
                    //std::cout<<ele_num<<std::endl;
                    //std::cout<<q_bins.size()<<std::endl;
                    size_t outSize=0;

                    auto cmprData=sz.encoding_lossless(outSize,q_bins);
                    delete []cmprData;
                    //std::cout<<"step 5"<<std::endl;
                    //std::cout<<outSize<<std::endl;
                    double bitrate=8*double(outSize)/ele_num;
                    bitrate+=8*sizeof(T)*anchor_rate;
                    if(conf.profiling){
                            bitrate*=((double)num_filtered_blocks)/(totalblock_num);
                        }

                    if(conf.tuningTarget==QoZ::TUNING_TARGET_RD){
                        double mse=square_error/ele_num;
                        if(conf.profiling){
                            mse*=((double)num_filtered_blocks)/(totalblock_num);
                         }
                         metric=QoZ::PSNR(rng,mse);
                    }
                    else if (conf.tuningTarget==QoZ::TUNING_TARGET_AC){
                       
                        metric=1.0-QoZ::autocorrelation<T>(flattened_sampled_data.data(),flattened_cur_blocks.data(),ele_num);
                        std::vector<T>().swap(flattened_cur_blocks);
                        
                    }
                  
                   std::vector<std::vector<int> >().swap( block_q_bins);
                    std::vector<size_t>().swap( q_bin_counts);

                    if ( (conf.tuningTarget!=QoZ::TUNING_TARGET_CR and metric>=bestm and bitrate<=bestb) or (conf.tuningTarget==QoZ::TUNING_TARGET_CR and bitrate<=bestb ) ){
                        bestalpha=alpha;
                        bestbeta=beta;
                        bestb=bitrate;
                        bestm=metric;
                        //printf("Best: %.2f %.2f %.4f %.2f\n",bestalpha,bestbeta,bestb,bestm);
                    }
                    else if ( (conf.tuningTarget!=QoZ::TUNING_TARGET_CR and metric<=bestm and bitrate>=bestb) or (conf.tuningTarget==QoZ::TUNING_TARGET_CR and bitrate>bestb) ){
                        if ( (alpha>=1 and pow(alpha,level_num-1)<=beta) or (alpha<1 and alpha*(level_num-1)<=beta) )
                            break;

                        continue;
                    }

                    else{
                        double orig_eb=conf.absErrorBound;
                        double eb_fixrate;
                        if (metric>bestm)
                            eb_fixrate=1.2; 
                        else
                            eb_fixrate=0.8;
                        conf.absErrorBound=orig_eb*eb_fixrate;
                        q_bins.clear();
                        block_q_bins.clear();
                        square_error=0.0;
                        double metric_r=0.0;
                        size_t idx=0;
                        for (size_t k =0;k<num_sampled_blocks;k++){
                            cur_block=sampled_blocks[k];
                            size_t tempsize;
                            SZ_compress_AutoSelectiveInterp_with_sampling<T,N>(conf,cur_block.data(),tempsize,op_candidates,dir_candidates,conf.sampleBlockSampleBlockSize,1);
                            //SZ_compress_AutoSelectiveInterp<T,N>(conf, cur_block.data(), tempsize,op_candidates,dir_candidates,1);
                            block_q_bins.push_back(conf.quant_bins);
                            square_error+=conf.decomp_square_error;

                            if (conf.tuningTarget==QoZ::TUNING_TARGET_SSIM){
                                double mean=0,sigma2=0,cov=0,range=0;

                                double orig_mean=0,orig_sigma2=0,orig_range=0;
                                if(N==2){
                                    for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                                        for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                                            orig_mean=orig_means[idx];
                                            orig_sigma2=orig_sigma2s[idx];
                                            orig_range=orig_ranges[idx];
                                            std::vector<size_t> starts{i,j};
                                            QoZ::blockwise_profiling<T>(cur_block.data(),conf.dims,starts,ssim_size,mean,sigma2,range);
                                            cov=QoZ::blockwise_cov<T>(sampled_blocks[k].data(),cur_block.data(),conf.dims,starts,ssim_size,orig_mean,mean);
                                            metric_r+=QoZ::SSIM(orig_range,orig_mean,orig_sigma2,mean,sigma2,cov)/ssim_block_num;
                                            idx++;
                                       }
                                    }
                                }

                                else if(N==3){
                                    for (size_t i=0;i+ssim_size<sampleBlockSize+1;i+=ssim_size){
                                        for (size_t j=0;j+ssim_size<sampleBlockSize+1;j+=ssim_size){
                                            for (size_t kk=0;kk+ssim_size<sampleBlockSize+1;kk+=ssim_size){
                                                orig_mean=orig_means[idx];
                                                orig_sigma2=orig_sigma2s[idx];
                                                orig_range=orig_ranges[idx];
                                                std::vector<size_t> starts{i,j,kk};
                                                QoZ::blockwise_profiling<T>(cur_block.data(),conf.dims,starts,ssim_size,mean,sigma2,range);
                                                cov=QoZ::blockwise_cov<T>(sampled_blocks[k].data(),cur_block.data(),conf.dims,starts,ssim_size,orig_mean,mean);
                                                metric_r+=QoZ::SSIM(orig_range,orig_mean,orig_sigma2,mean,sigma2,cov)/ssim_block_num;
                                                idx++;
                                            }
                                        }
                                    }
                                }



                            }
                            else if (conf.tuningTarget==QoZ::TUNING_TARGET_AC){
                                flattened_cur_blocks.insert(flattened_cur_blocks.end(),cur_block.begin(),cur_block.end());
                            }
                        }
                        conf.absErrorBound=orig_eb;
                        q_bin_counts=conf.quant_bin_counts;
                        level_num=q_bin_counts.size();
                        last_pos=0;
                        for(int k=level_num-1;k>=0;k--){
                            for (size_t l =0;l<num_sampled_blocks;l++){
                                for (size_t m=last_pos;m<q_bin_counts[k];m++){
                                    q_bins.push_back(block_q_bins[l][m]);
                                }
                            }
                            last_pos=q_bin_counts[k];
                        }
                        
                        outSize=0;
                        auto cmprData=sz.encoding_lossless(outSize,q_bins);
                        //delete sz;
                        delete []cmprData;          
                        double bitrate_r=8*double(outSize)/ele_num;
                        bitrate_r+=8*sizeof(T)*anchor_rate;
                        if(conf.profiling){
                            bitrate_r*=((double)num_filtered_blocks)/(totalblock_num);
                        }

                        if(conf.tuningTarget==QoZ::TUNING_TARGET_RD){
                            double mse=square_error/ele_num;
                            if(conf.profiling){
                                mse*=((double)num_filtered_blocks)/(totalblock_num);
                             }
                             metric_r=QoZ::PSNR(rng,mse);
                        }
                        else if (conf.tuningTarget==QoZ::TUNING_TARGET_AC){
                       
                            metric=1.0-QoZ::autocorrelation<T>(flattened_sampled_data.data(),flattened_cur_blocks.data(),ele_num);
                            std::vector<T>().swap(flattened_cur_blocks);
                            
                        }
                    
                        double a=(metric-metric_r)/(bitrate-bitrate_r);
                        double b=metric-a*bitrate;
                        double reg=a*bestb+b;
                        //printf("%.2f %.2f %.4f %.2f\n",alpha,beta,bitrate_r,metric_r);
                        //printf("%.2f %.2f %.4f %.2f\n",alpha,beta,bestb,reg);                       
                        //conf.absErrorBound=orig_eb;
                        if (reg>bestm){
                            bestalpha=alpha;
                            bestbeta=beta;               
                            bestb=bitrate;
                            bestm=metric;
                            //printf("Best: %.2f %.2f %.4f %.2f\n",bestalpha,bestbeta,bestb,bestm);
                        }
                        std::vector<int>().swap( q_bins);
                        std::vector<std::vector<int> >().swap( block_q_bins);
                        std::vector<size_t>().swap( q_bin_counts);
                    }

                    if ( (alpha>=1 and pow(alpha,level_num-1)<=beta) or (alpha<1 and alpha*(level_num-1)<=beta) )
                        break;
                }
            }

            conf.alpha=bestalpha;
            conf.beta=bestbeta;
            conf.dims=global_dims;
            conf.num=global_num;
            conf.interpAlgo_list.clear();
            conf.interpDirection_list.clear();
            //delete sz;           
            if(conf.tuningTarget==QoZ::TUNING_TARGET_AC){
                bestm=1-bestm;
            }
            std::string metric_name="no";
            if (conf.tuningTarget==QoZ::TUNING_TARGET_RD ){
                metric_name="PSNR";
            }
            else if (conf.tuningTarget==QoZ::TUNING_TARGET_SSIM ){
                metric_name="SSIM";
            }
            else if (conf.tuningTarget==QoZ::TUNING_TARGET_AC ){
                metric_name="AutoCorrelation";
            }
            if(conf.verbose)
                printf("Autotuning finished. Selected alpha: %f. Selected beta: %f. Best bitrate: %f. Best %s: %f.\n", bestalpha,bestbeta,bestb, const_cast<char*>(metric_name.c_str()),bestm);
            /*
            for(int i=0;i<conf.num;i++)
                data[i]=orig_data[i];
              */  
        }   

    }

   // bool useInterp = !(best_lorenzo_ratio > best_interp_ratio && best_lorenzo_ratio < 80 && best_interp_ratio < 80);
    
//    printf("\nLorenzo compression ratio = %.2f\n", best_lorenzo_ratio);
//    printf("Interp compression ratio = %.2f\n", best_interp_ratio);
     {
        //conf.cmprAlgo = QoZ::ALGO_INTERP;
        //double tuning_time = timer.stop();
        conf.blockwiseTuning=1;

//        std::cout << "Tuning time = " << tuning_time << "s" << std::endl;
        auto sz = QoZ::SZInterpolationCompressor<T, N, QoZ::LinearQuantizer<T>, QoZ::HuffmanEncoder<int>, QoZ::Lossless_zstd>(
            QoZ::LinearQuantizer<T>(conf.absErrorBound),
            QoZ::HuffmanEncoder<int>(),
            QoZ::Lossless_zstd());

        char *cmpData = (char *) sz.compress_block(conf, data, outSize);
        return cmpData;  
    } 
}

#endif