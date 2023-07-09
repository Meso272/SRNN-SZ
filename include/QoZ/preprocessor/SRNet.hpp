#ifndef SZ3_SRNET_HPP
#define SZ3_SRNET_HPP
#include "QoZ/utils/FileUtil.hpp"
#include <cstdlib>
#include<cmath>
template<class T, QoZ::uint N>
T * super_resolution(T *lr_data, const std::array<size_t,N> &lr_dims,int scale=2){
    size_t lr_num=1;
    for(uint i=0;i<N;i++)
        lr_num+=lr_dims[i];


    std::string HOME = getenv("HOME");

    std::string HAT_root=HOME+"/lossycompression/HAT";

    std::string YML_path=HAT_root+"/options/test";

    std::string YML_template_path=YML_path+"/qoz_template.yml";
    std::string YML_file_path=YML_path+"/qoz.yml";

    std::string Dataset_path=HAT_root+"/datasets/qoz";
    std::string Datafile_path=Dataset_path+"/qoz.dat";

    std::string yml_generation_command="sed \'s/size_x/size_x: "+ std::to_string(lr_dims[0]) + "/g\' "+ YML_template_path +">" + YML_file_path + 
                                        "&&sed -i \'s/size_y/size_y: " + std::to_string(lr_dims[1]) + "/g\' "+YML_file_path;
    system(yml_generation_command);
    QoZ::writefile<T>(Datafile_path.c_str(), lr_data, lr_num);

    std::string SRNet_command="cd "+HAT_root+"&& python hat/test.py -opt options/test/"+YML_file_path;
    system(SRNet_command);
    std::string Result_folder=HAT_root+"/results/HAT_SRx2_4QoZ";
    std::string HR_path=Result_folder+"/visualization/qoz/qoz_HAT_SRx2_4QoZ.dat";
    size_t hr_num=lr_num*pow(scale,N);
    T* hr_data=new T[hr_num];
    QoZ::readfile<T>(HR_path.c_str(), hr_num,hr_data);

    std::string Clean_command="rm -f "+Datafile_path+";rm -rf "+Result_folder;
    system(Clean_command);

    return hr_data;






}




#endif //SZ3_SRNET_HPP