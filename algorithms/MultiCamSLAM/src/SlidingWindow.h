#ifndef MCS_SLIDING_WINDOW_H
#define MCS_SLIDING_WINDOW_H
#include <deque>
#include <memory>
#include <mutex>
#include "Frame.h"


#include <glog/logging.h>

#include <gtsam/base/Matrix.h>
#include <gtsam/base/Vector.h>

#include <gtsam/inference/Key.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/GaussNewtonOptimizer.h>


#include <gtsam/inference/Ordering.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>

#include <gtsam/linear/Preconditioner.h>
#include <gtsam/linear/PCGSolver.h>

#include <gtsam/nonlinear/DoglegOptimizer.h>

#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/nonlinear/NonlinearISAM.h>
#include <gtsam/nonlinear/NonlinearEquality.h>

#include <gtsam/slam/dataset.h>
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam/nonlinear/ISAM2Params.h>
#include <gtsam/slam/SmartFactorParams.h> //like chi2 outlier select.
#include <gtsam/slam/GeneralSFMFactor.h>

#include <gtsam/geometry/Cal3_S2Stereo.h>
#include <gtsam/slam/StereoFactor.h>
#include <gtsam_unstable/slam/SmartStereoProjectionPoseFactor.h>

#include <gtsam_unstable/nonlinear/ConcurrentBatchFilter.h>
#include <gtsam_unstable/nonlinear/ConcurrentBatchSmoother.h>
#include <gtsam_unstable/nonlinear/BatchFixedLagSmoother.h>


#include <gtsam/slam/ProjectionFactor.h>
#include <gtsam/slam/PoseTranslationPrior.h>
#include <gtsam/base/numericalDerivative.h>
#include <gtsam/base/serializationTestHelpers.h>
//#include <gtsam/geometry/Rot3.h>
//#include <gtsam/geometry/Point3.h>
//#include <gtsam/geometry/Pose3.h>


#include <opencv2/core/persistence.hpp>
#include <opencv2/core/eigen.hpp>
#include "opencv2/features2d.hpp"
#include "opencv2/xfeatures2d.hpp"
#include "opencv2/video/tracking.hpp"
#include "opencv2/calib3d.hpp"


#include "FrameManager.h"

#include <thread>
#include <opencv2/core/eigen.hpp>
#include "IMU_Preint_GTSAM.h"
#include "FrameWiseGeometry.h"
#include <iostream>
#include "Visualization.h"
#include "ReprojectionInfoDatabase.h"

using namespace gtsam;
using namespace std;
using namespace cv;

namespace mcs
{


//滑动窗口类
class SlidingWindow
{
private:
    //deque<shared_ptr<Frame> > ordinaryFrameQueue;
    //deque<shared_ptr<Frame> > KF_queue;
    //vector<weak_ptr<LandmarkProperties> vLandmarks;//shared_ptr保存在对应的kf里.

    //扩展kf的内容.这里不需要外部Frame结构知道这种扩展.
    //map<int,LandmarkProperties> KF_landmark_extension_map;
    ReprojectionInfoDatabase reproj_db;
    const int max_kf_count = 5;
    deque<int> KF_id_queue;

public:
    SlidingWindow()
    {
    }
    shared_ptr<Frame> getFrameByID(int frame_id)
    {
        return this->reproj_db.frameTable.query(frame_id);
    }
    inline shared_ptr<Frame> getLastKF()
    {
        return getFrameByID(KF_id_queue.end());
    }
    vector<int> getKFidQueue()
    {
        return this->KF_id_queue;
    }
    shared_ptr<Values> optimizeFactorGraph(shared_ptr<NonlinearFactorGraph> pGraph,shared_ptr<Values> pInitialEstimate)
    {
        ScopeTimer timer_optimizer("optimizeFactorGraph()");
        //初始化一个Levenburg-Marquardt优化器,解优化图.返回估计值.
        LevenbergMarquardtOptimizer optimizer(*pGraph, *pInitialEstimate, params);
        Values *pResult = new Values();
        *pResult = optimizer.optimize();
        cout<<"optimized result:"<<endl;
        pResult->print();
        return shared_ptr<Values>(pResult);
    }
    void trackAndKeepReprojectionDBForFrame(shared_ptr<Frame> pFrame)//这是普通帧和关键帧公用的.
    {
        this->reproj_db.frameTable.insertFrame(pFrame);//加入frame_id;
        for(const int& ref_kf_id:this->getInWindKFidVec())//对当前帧,追踪仍在窗口内的关键帧.
        {
            //第一步 跟踪特征点,创建关联关系.
            auto pRefKF = getFrameByID(ref_kf_id);
            const int cam_count = pFrame->get_cam_num();
            vector<vector<Point2f> > vvLeftp2d;//,vvRightp2d;
            vector<vector<float> > vv_disps;//移除vvRightp2d.
            vector<vector<char> > vv_track_type;//跟踪成功与否,跟踪的结果.

            vvLeftp2d.resize(cam_count);vv_disps.resize(cam_count);vv_track_type.resize(cam_count);//vvRightp2d.resize(cam_count);
            vector<map<int,int> > v_p2d_to_kf_p3d_index,v_p2d_to_kf_p2d_index;
            v_p2d_to_kf_p3d_index.resize(cam_count);v_p2d_to_kf_p2d_index.resize(cam_count);
            vector<vector<Point2f> >  v_originalKFP2d_relative;//对应的关键帧p2d.
            v_originalKFP2d_relative.resize(cam_count);
            vector<char> v_track_success;
            v_track_success.resize(cam_count);
            vector<int> v_track_success_count;
            v_track_success_count.resize(cam_count);
            for(int i = 0;i<cam_count;i++)//这里可以多线程.暂时不用.
            {
                //doTrackLastKF(pFrame,getLastKF(),i,vvLeftp2d.at(i),vvRightp2d.at(i),v_p2d_to_kf_p3d_index.at(i),v_originalKFP2d_relative.at(i),&v_track_success.at(i),&v_track_success_count.at(i));
                //doTrackLastKF(...);//跟踪窗口里的所有kf.处理所有情况(mono2mono,mono2stereo,stereo2mono,stereo2stereo.)
                //TODO:start a thread rather than invoke doTrackLastKF_all2dpts() directly.
                doTrackLaskKF_all2dpts(pFrame,getFrameByID(ref_kf_id),i,
                                       this->getFrameByID(ref_kf_id).p2d_vv.at(i),
                                       vv_disps.at(i),vv_track_type.at(i),v_p2d_to_kf_p2d_index.at(i),v_p2d_to_kf_p3d_index.at(i),
                                       v_track_success.at(i),v_track_success_count.at(i)
                                       );
            }
            //threads.join();//合并结果集.

            pFrame->reproj_map.at(ref_kf_id) = ReprojectionRecordT();
            pFrame->reproj_map.at(ref_kf_id).resize(cam_count);
            //改Frame之间引用关系.
            //是不是上一帧?
            pFrame->setReferringID(ref_kf_id);//最后一个是上一个.

            //第二步 建立地图点跟踪关系 根据结果集维护数据库.
            for(int i = 0 ;i<cam_count,i++)//这是一个不可重入过程.数据库本身访问暂时没有锁.暂时估计应该不需要.
            {
                if(v_track_success.at(i))
                {
                    //反复查表,改数据库.
                    //pFrame->reproj_map.at(ref_kf_id).at(i).push_back();
                    //        this->reproj_db.
                    //step<1>.改frame和相应ReprojectionInfo结构
                    for(int current_frame_p2d_index = 0;current_frame_p2d_index<vvLeftp2d.at(i).size();current_frame_p2d_index++)
                    {
                        SingleProjectionT proj_;
                        proj_.current_frame_p2d = vvLeftp2d.at(i).at(current_frame_p2d_index);
                        proj_.disp = vv_disps.at(i).at(current_frame_p2d_index);
                        proj_.ref_p2d_id = v_p2d_to_kf_p2d_index.at(i).at(current_frame_p2d_index);
                        proj_.tracking_state = vv_track_type.at(i).at(current_frame_p2d_index);
                        pFrame->reproj_map.at(ref_kf_id).at(i).at(current_frame_p2d_index) = proj_;
                        //查询是否存在对应的 landmark结构.如果不存在,创建一个.
                        if(proj_.tracking_state == TRACK_STEREO2STEREO|| proj_.tracking_state == TRACK_STEREO2MONO||proj_.tracking_state == TRACK_MONO2STEREO
                                //||proj_.tracking_state == TRACK_MONO2MONO // 暂时不管.

                                )//如果这个点可直接三角化.
                        {
                            if(pRefKF->kf_p2d_to_landmark_id.at(i).count(proj_.ref_p2d_id) == 0)
                            {//创建landmark
                                shared_ptr<LandmarkProperties> pLandmark(new LandmarkProperties());
                                pLandmark->cam_id = i;
                                pLandmark->created_by_kf_id = ref_kf_id;
                                pLandmark->landmark_reference_time = 1; //创建那一次不算.
                                pLandmark->relative_kf_p2d_id = proj_.ref_p2d_id;
                                this->reproj_db.landmarkTable.insertLandmark(pLandmark);//维护索引.从此以后它就有id了.
                                if(proj_.tracking_state == TRACK_STEREO2MONO||proj_.tracking_state == TRACK_STEREO2STEREO)
                                {
                                    pLandmark->setEstimatedPosition(pRefKF->);//这里认为参考的关键帧位姿已经优化完毕.可以直接根据坐标变换关系计算Landmark位置初始估计.
                                    pLandmark->setTriangulated();//三角化成功.
                                }
                                else if(proj_.tracking_state == TRACK_MONO2STEREO)
                                {//这种比较特殊.暂时认为当前帧位姿 == 最后一个被优化的帧位姿,进行三角化;这种误差肯定是比上一种情况要大.
                                    pLandmark->setEstimatedPosition(p);
                                    pLandmark->setTriangulated();
                                }
                                else if(proj_.tracking_state == TRACK_MONO2MONO)//这种三角化最复杂.应该放到一个队列里去.判断是否能够三角化.实用意义不大.
                                {
                                    //TriangulationQueue.push_back( std::make_tuple<pLandmark->landmark_id,pFrame->frame_id,ref_kf_id)//加入队列.等待处理.
                                    //在当前帧优化成功之后,检查对极约束是否满足,夹角是否足够三角化....
                                    //pLandmark->setEstimatedPosition();
                                    LOG(WARNING)<<"Track mono2mono not implemented yet!"<<endl;
                                }
                                pLandmark->ObservedByFrameIDSet.insert(pFrame->frame_id);//当前帧id加入观测关系.


//                                ObservationInfo obs_info;
//                                obs_info.observed_by_frame_id = pFrame->frame_id;
//                                obs_info.relative_p2d_index = ;//TODO:这块逻辑整理下?

//                                pLandmark->vObservationInfo.push_back();
                            }
                            else
                            {//维护landmark.
                                //ObservationInfo obs_info;....//这块用到的时候设计一下.
                            }
                        }
                    }
                    //* 可选 加入RelationTable,并更新对应的索引关系.
                }
                else
                {//防止出现空结构无法访问.
                    //TODO.
                }
            }
        }
    }
    void insertKFintoSlidingWindow(shared_ptr<Frame> pCurrentKF)
    {
        //第一步 跟踪特征点,创建关联关系.
        //第二步 建立地图点跟踪关系 根据结果集维护数据库.
        trackAndKeepReprojectionDBForFrame(pCurrentKF);
        //第三步 将当前普通帧升级成一个关键帧.
        upgradeOrdinaryFrameToKeyFrameStereos(pCurrentKF);//升级.
        //第四步 创建局部优化图 第一次优化.
        //method<1>.pnp初始位置估计.
        //  stage<1>.选取最优相机组,估计初始位置.
        //method<2>.继承上一帧初始位置.直接图优化和上一帧的关系.
//        for(int i = 0;i<cam_count;i++)
//        {//创建对应的X,L;插入初始值(X位姿估计在上一个估计点;L位置估计在对应X的坐标变换后位置);Between Factor<>,每次对滑窗里最早的帧约束位置关系.
//            int pose_index = x*pCurrentKF->frame_id + i;
//            localGraph.emplace_shared<Pose3>(...);
//            localGraph.add();
//            localInitialEstimate.insert(...);
//        }

        shared_ptr<Values> pLocalInitialEstimate,pLocalRes;
        shared_ptr<NonlinearFactorGraph> pLocalGraph = this->reproj_db.generateLocalGraphByFrameID(pCurrentKF->frame_id,pLocalInitialEstimate);//优化当前帧.
        pLocalRes = optimizeFactorGraph(pLocalGraph,pLocalInitialEstimate);//TODO:这种"优化" 可以考虑多线程实现.
        //优化这个图.
        //第五步 对当前帧,跟踪滑窗里的所有关键帧(地图点向当前帧估计位置重投影).创建新优化图.

        shared_ptr<Values> pSWRes,pSWInitialEstimate;
        shared_ptr<NonlinearFactorGraph> pSlidingWindGraph = this->reproj_db.generateCurrentGraphByKFIDVector(this->getInWindKFidVec(),pSWInitialEstimate);
        pSWRes = optimizeFactorGraph(pSlidingWindGraph,pSWInitialEstimate);
        //第六步 第二次优化.

        //第七步 进行Marginalize,分析优化图并选择要舍弃的关键帧和附属的普通帧,抛弃相应的信息.
        int toMargKFID = this->proposalMarginalizationKF();
        if(toMargKFID < 0)//无需marg.
        {
            LOG(INFO)<<"deque not full, skip marginalization.return."<<endl;
            return;
        }
        shared_ptr<Frame> pToMarginalizeKF = this->getFrameByID(toMargKFID);
        if(pToMarginalize!= nullptr)
        {
            //TODO:对它的每一个从属OrdinaryFrame进行递归删除.
            if(this->toMargKFID == this->KF_id_queue.back())
            {
                this->KF_id_queue.pop_back();
            }
            else if(this->toMargKFID == this->KF_id_queue.front())
            {
                this->KF_id_queue.pop_front();
            }
            else
            {
                LOG(ERROR)<<"error in proposalMarginalizationKF()"<<endl;
                exit(-1);
            }
            removeKeyFrameAndItsProperties(pToMarginalizeKF);
            removeOrdinaryFrame(pToMarginalizeKF);
        }
    }

    void insertOrdinaryFrameintoSlidingWindow(shared_ptr<Frame> pCurrentFrame)
    //普通帧和关键帧的区别是:普通帧没有附属landmark properties;普通帧与普通帧之间不考虑关联追踪.
    {
        //第一步 跟踪特征点.
        //第二步 建立地图点跟踪关系. 修改/创建LandmarkProperties.
        trackAndKeepReprojectionDBForFrame(pOrdinaryFrame);
        //第三步 创建局部优化图.第一次优化.
        shared_ptr<NonlinearFactorGraph> pLocalGraph = this->reproj_db.generateLocalGraphByFrameID(pOriginaryFrame->frame_id);
    }

    void removeOrdinaryFrame(shared_ptr<Frame>);
    void removeKeyFrameAndItsProperties(shared_ptr<Frame>);
    inline int getOrdinaryFrameSize();
    inline int getKFSize();
    vector<int> getInWindKFidVec();
    int proposalMarginalizationKF(int currentFrameID)//提议一个应该被marg的关键帧.
    {
        auto pCurrentFrame = this->getFrameByID(currentFrameID);
        deque<int> currentInWindKFList = this->getKFidQueue();
        if(currentInWindKFList.size() < this->max_kf_count)
        {
            return -1;
        }
        vector<double> v_mean_disp;
        for(auto iter = currentInWindKFList.begin();iter!=currentInWindKFList.end();++iter)
        {
            int kf_id = *iter;
            vector<shared_ptr<Frame> > v_relative_frames = this->reproj_db.frameTable.queryByRefKFID(kf_id);//查询和他有关联的帧.
            for(auto& pF:v_relative_frames)
            {//判断marg哪个关键帧最合适.
                //step<1>.计算帧间平均视差.
                double mean_disp = calcMeanDispBetween2Frames(pCurrentFrame,pF);
            }
            //step<2>.如果第一帧到当前帧平均视差 与 最后一帧到当前帧平均视差 比值<2.0: marg最后一个关键帧
            //    (一直不动.就不要创建新的.否则会一直累计误差.)

            //step<3>.否则,marg第一个关键帧.
        }
        if(v_mean_disp[0]<2* (*v_mean_disp.end()) )
        {
            return currentInWindKFList.begin();
        }
        return currentInWindKFList.end();
    }
};








}
#endif