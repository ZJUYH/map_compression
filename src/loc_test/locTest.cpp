#include "ros/ros.h"
#include "ros/console.h"

#include "pointmatcher/PointMatcher.h"
#include "pointmatcher/Timer.h"
#include "pointmatcher_ros/point_cloud.h"
#include "pointmatcher_ros/transform.h"
#include "nabo/nabo.h"
#include "eigen_conversions/eigen_msg.h"
#include "pointmatcher_ros/get_params_from_server.h"

#include <tf/transform_broadcaster.h>

#include <fstream>
#include <vector>
#include <algorithm>

#include <visualization_msgs/Marker.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Path.h>

#include <numeric>

using namespace std;
using namespace PointMatcherSupport;

class locTest
{
    typedef PointMatcher<float> PM;
    typedef PM::DataPoints DP;
    typedef PM::Matches Matches;

    typedef typename Nabo::NearestNeighbourSearch<float> NNS;
    typedef typename NNS::SearchType NNSearchType;

public:
    locTest(ros::NodeHandle &n);
    ~locTest();
    ros::NodeHandle& n;

    string wholeMapName;
    string icpFileName;
    string velodyneDirName;

    string savePoseName;
    string saveTimeName;
    string saveIterName;

    string icpYaml;
    string inputFilterYaml;
    string mapFilterYaml;

    DP mapCloud;
    DP velodyneCloud;

    double deltaTime;
    int iterCount;

    int startIndex;
    int endIndex;

    bool isKITTI;
    bool isChery;

    //icp
    PM::DataPointsFilters inputFilters;
    PM::DataPointsFilters mapFilters;
    PM::ICPSequence icp;
    PM::TransformationParameters Ticp;
    PM::TransformationParameters Tinit;
    PM::TransformationParameters Ttemp;

    vector<vector<double>> initPoses;

    void process(int cnt);
    DP readYQBin(string filename);
    DP readKITTIBin(string filename);

    ros::Publisher mapCloudPub;
    ros::Publisher velCloudPub;
    tf::TransformBroadcaster tfBroadcaster;

    shared_ptr<NNS> NNSMap;
    unique_ptr<PM::Transformation> transformation;
};

locTest::~locTest()
{}

locTest::locTest(ros::NodeHandle& n):
    n(n),
    wholeMapName(getParam<string>("wholeMapName", ".")),
    icpFileName(getParam<string>("icpFileName", ".")),
    startIndex(getParam<int>("startIndex", 0)),
    endIndex(getParam<int>("endIndex", 0)),
    icpYaml(getParam<string>("icpYaml", ".")),
    velodyneDirName(getParam<string>("velodyneDirName", ".")),
    savePoseName(getParam<string>("savePoseName", ".")),
    saveTimeName(getParam<string>("saveTimeName", ".")),
    saveIterName(getParam<string>("saveIterName", ".")),
    inputFilterYaml(getParam<string>("inputFilterYaml", ".")),
    mapFilterYaml(getParam<string>("mapFilterYaml", ".")),
    isKITTI(getParam<bool>("isKITTI", 0)),
    isChery(getParam<bool>("isChery", 0)),
    transformation(PM::get().REG(Transformation).create("RigidTransformation"))
{
    mapCloudPub = n.advertise<sensor_msgs::PointCloud2>("map_cloud", 2, true);
    velCloudPub = n.advertise<sensor_msgs::PointCloud2>("velodyne_cloud", 2, true);

    // load
    mapCloud = DP::load(wholeMapName);

    mapCloud.removeDescriptor("normals");
    mapCloud.removeDescriptor("eigValues");

    ifstream mapFilterss(mapFilterYaml);
    mapFilters = PM::DataPointsFilters(mapFilterss);
    mapFilters.apply(mapCloud);

    NNSMap.reset(NNS::create(mapCloud.features, mapCloud.features.rows() - 1, NNS::KDTREE_LINEAR_HEAP, NNS::TOUCH_STATISTICS));

    cout<<"Need Re-filter"<<endl;

    // read initial transformation
    int x, y;
    double temp;
    vector<double> test;
    ifstream in(icpFileName);
    if (!in) {
        cout << "Cannot open file.\n";
    }
    for (y = 0; y < 99999; y++) {
        test.clear();
        if(in.eof()) break;
    for (x = 0; x < 16; x++) {
      in >> temp;
      test.push_back(temp);
    }
      initPoses.push_back(test);
    }
    in.close();

    cout<<"start locTest"<<endl;

    // initial icp
    ifstream ifs(icpYaml);
    icp.loadFromYaml(ifs);
    icp.setMap(mapCloud);

    ofstream saverTime(saveTimeName);
    ofstream saverLoc(savePoseName);
    ofstream saverIter(saveIterName);

    mapCloudPub.publish(PointMatcher_ros::pointMatcherCloudToRosMsg<float>(mapCloud, "global", ros::Time::now()));

    // process
    // from start to end
    for(int cnt = startIndex; cnt<=endIndex; cnt++)
    {
        this->process(cnt);

        //saving...
        if(cnt!=startIndex)
        {
            saverTime<<deltaTime<<endl;
            saverIter<<iterCount<<endl;
            saverLoc<<Ticp(0,0)<<"  "<<Ticp(0,1)<<"  "<<Ticp(0,2)<<"  "<<Ticp(0,3)<<"  "
                      <<Ticp(1,0)<<"  "<<Ticp(1,1)<<"  "<<Ticp(1,2)<<"  "<<Ticp(1,3)<<"  "
                     <<Ticp(2,0)<<"  "<<Ticp(2,1)<<"  "<<Ticp(2,2)<<"  "<<Ticp(2,3)<<"  "
                    <<Ticp(3,0)<<"  "<<Ticp(3,1)<<"  "<<Ticp(3,2)<<"  "<<Ticp(3,3)<<endl;
            // publish
            tfBroadcaster.sendTransform(PointMatcher_ros::eigenMatrixToStampedTransform<float>(Ticp, "global", "robot", ros::Time::now()));
            velCloudPub.publish(PointMatcher_ros::pointMatcherCloudToRosMsg<float>(velodyneCloud, "robot", ros::Time::now()));
        }
    }
}

void locTest::process(int cnt)
{

    cout<<cnt<<endl;

    // first time, just set the initial value
    if(cnt == startIndex)
    {
        cout<<"!!!!!!"<<endl;
        Tinit = PM::TransformationParameters::Identity(4, 4);
        Tinit(0,0)=initPoses[cnt][0];Tinit(0,1)=initPoses[cnt][1];Tinit(0,2)=initPoses[cnt][2];Tinit(0,3)=initPoses[cnt][3];
        Tinit(1,0)=initPoses[cnt][4];Tinit(1,1)=initPoses[cnt][5];Tinit(1,2)=initPoses[cnt][6];Tinit(1,3)=initPoses[cnt][7];
        Tinit(2,0)=initPoses[cnt][8];Tinit(2,1)=initPoses[cnt][9];Tinit(2,2)=initPoses[cnt][10];Tinit(2,3)=initPoses[cnt][11];
        Tinit(3,0)=initPoses[cnt][12];Tinit(3,1)=initPoses[cnt][13];Tinit(3,2)=initPoses[cnt][14];Tinit(3,3)=initPoses[cnt][15];
        return;
    }

    if(!isChery)
    {
        stringstream ss;
        if(isKITTI)
            ss<<setw(6)<<setfill('0')<<cnt;
        else
            ss<<setw(10)<<setfill('0')<<cnt;

        string str;
        ss>>str;
        string veloName = velodyneDirName + str + ".bin";
        cout<<veloName<<endl;

        if(isKITTI)
            velodyneCloud = this->readKITTIBin(veloName);       // KITTI dataset
        else
            velodyneCloud = this->readYQBin(veloName);  // YQ dataset
    }
    else
    {
        string vtkFileName = velodyneDirName + std::to_string(cnt) + ".vtk";
        velodyneCloud = DP::load(vtkFileName);  // Chery dataset
    }

    cout<<"-----------------------------------------------------------------------"<<endl;
    cout<<"VEL_NUM:  "<<velodyneCloud.features.cols()<<endl;

    ifstream inputFilterss(inputFilterYaml);
    inputFilters = PM::DataPointsFilters(inputFilterss);
    inputFilters.apply(velodyneCloud);

    cout<<"filtered VEL_NUM:  "<<velodyneCloud.features.cols()<<endl;

    // icp
    // bug "Ignore..." fixed, quat!
    Eigen::Matrix3f BaseToMapRotation = Tinit.block(0,0,3,3);
    Eigen::AngleAxisf BaseToMapAxisAngle(BaseToMapRotation);    // RotationMatrix to AxisAngle
    Tinit.block(0,0,3,3) = BaseToMapAxisAngle.toRotationMatrix();

    Ttemp=Tinit;

    double t0 = ros::Time::now().toSec();

    Ticp = icp(velodyneCloud, Tinit);

    double t1 = ros::Time::now().toSec();

    this->deltaTime = t1-t0;
    this->iterCount = icp.maxIteration;
//    this->iterCount = 0;

    cout<<"ICP...   "<<deltaTime<<endl;

    Tinit = Ticp;

    // save the results outside this function

    // color for matching results, optional
    // need transformation, label the matched points

    /*
    velodyneCloud.addDescriptor("matched", PM::Matrix::Zero(1, velodyneCloud.features.cols()));
    int rowLinemaMatched = velodyneCloud.getDescriptorStartingRow("matched");

    transformation->correctParameters(Ttemp);
    DP velodyneCloud_ = transformation->compute(velodyneCloud, Ttemp);

    PM::Matches matches_velo(
        Matches::Dists(5, velodyneCloud_.features.cols()),
        Matches::Ids(5, velodyneCloud_.features.cols())
    );
    NNSMap->knn(velodyneCloud_.features, matches_velo.ids, matches_velo.dists, 1, 0);

    for(int p=0; p<velodyneCloud_.features.cols(); p++)
    {
        if(sqrt(matches_velo.dists(0,p)) < 0.3)
            velodyneCloud.descriptors(rowLinemaMatched, p) = 1;
    }
    */

}

locTest::DP locTest::readYQBin(string filename)
{
    DP data;

    fstream input(filename.c_str(), ios::in | ios::binary);
    if(!input.good()){
        cerr << "Could not read file: " << filename << endl;
        exit(EXIT_FAILURE);
    }
    input.seekg(0, ios::beg);

    data.addFeature("x",PM::Matrix::Constant(1,300000,0));
    data.addFeature("y",PM::Matrix::Constant(1,300000,0));
    data.addFeature("z",PM::Matrix::Constant(1,300000,0));
    data.addFeature("pad",PM::Matrix::Constant(1,300000,1));
    data.addDescriptor("intensity",PM::Matrix::Constant(1,300000,0));
    data.addDescriptor("ring",PM::Matrix::Constant(1,300000,0));

    int i;

    for (i=0; input.good() && !input.eof(); i++) {
        float a,b;
        input.read((char *) &a, 4*sizeof(unsigned char));
        data.features(0,i) = a;
        input.read((char *) &a, 4*sizeof(unsigned char));
        data.features(1,i) = a;
        input.read((char *) &a, 4*sizeof(unsigned char));
        data.features(2,i) = a;
        input.read((char *) &b, 4*sizeof(unsigned char));
        input.read((char *) &a, 4*sizeof(unsigned char));
        data.descriptors(0,i) = a;
        input.read((char *) &a, 2*sizeof(unsigned char));
        data.descriptors(1,i) = a;

        input.read((char *) &b, 10*sizeof(unsigned char));

    }
    input.close();
    data.conservativeResize(i);

    return data;
}

//For kitti dataset
locTest::DP locTest::readKITTIBin(string fileName)
{
    DP tempScan;

    int32_t num = 1000000;
    float *data = (float*)malloc(num*sizeof(float));

    // pointers
    float *px = data+0;
    float *py = data+1;
    float *pz = data+2;
    float *pr = data+3;

    // load point cloud
    FILE *stream;
    stream = fopen (fileName.c_str(),"rb");
    num = fread(data,sizeof(float),num,stream)/4;

    //ethz data structure
    tempScan.addFeature("x", PM::Matrix::Zero(1, num));
    tempScan.addFeature("y", PM::Matrix::Zero(1, num));
    tempScan.addFeature("z", PM::Matrix::Zero(1, num));
    tempScan.addDescriptor("intensity", PM::Matrix::Zero(1, num));

    int x = tempScan.getFeatureStartingRow("x");
    int y = tempScan.getFeatureStartingRow("y");
    int z = tempScan.getFeatureStartingRow("z");
    int intensity = tempScan.getDescriptorStartingRow("intensity");


    for (int32_t i=0; i<num; i++)
    {
        tempScan.features(x,i) = *px;
        tempScan.features(y,i) = *py;
        tempScan.features(z,i) = *pz;
        tempScan.descriptors(intensity,i) = *pr;
        px+=4; py+=4; pz+=4; pr+=4;
    }
    fclose(stream);

    ///free the ptr
    {
        free(data);
    }

    return tempScan;
}


int main(int argc, char **argv)
{

    ros::init(argc, argv, "locTest");
    ros::NodeHandle n;

    locTest loctest(n);
	
	exit(0);

    return 0;
}
