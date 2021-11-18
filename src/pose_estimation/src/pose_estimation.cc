#include <pose_estimation/pose_estimation.hpp>

PoseEstimation::PoseEstimation(): nh_("~"), dynamic_reconfigure_server_(nh_)
{}

void PoseEstimation::Initialize(const ros::NodeHandle& nh)
{
  // Node handler
  nh_ = nh;
  // Dynamic reconfigure
  dynamic_reconfigure::Server<DynamicReconfigureType>::CallbackType f;
  f = boost::bind(&PoseEstimation::OnDynamicReconfigure, this, _1, _2);
  dynamic_reconfigure_server_.setCallback(f);
}

void PoseEstimation::OnImage(const sensor_msgs::ImageConstPtr& img_rgb_msg, const sensor_msgs::ImageConstPtr& img_depth_msg)
{
	cv_bridge::CvImagePtr cv_ptr;
	cv_bridge::CvImagePtr cv_ptr_depth;

	try
	{
		cv_ptr = cv_bridge::toCvCopy(img_rgb_msg, sensor_msgs::image_encodings::BGR8);
		cv_ptr_depth = cv_bridge::toCvCopy(img_depth_msg, sensor_msgs::image_encodings::TYPE_32FC1);
	}
	catch (cv_bridge::Exception& e)
	{
		ROS_ERROR("cv_bridge exception: %s", e.what());
	}

  // Copy images
  cv_ptr->image.copyTo(img_rgb);
  cv_ptr_depth->image.copyTo(img_depth);

  // Get diff image
  cv::absdiff(img_rgb, img_empty_background, img_diff);

  // Apply mask
  img_diff_masked = detector::ApplyMask(img_diff, config_.mask_x, config_.mask_y, config_.mask_w, config_.mask_h);

  // Grey
  cv::cvtColor(img_diff_masked, img_diff_masked_grey, cv::COLOR_BGR2GRAY);

  // Threshold back projected image to create a binary mask og the projected image
  cv::threshold(img_diff_masked_grey, img_binary, config_.threshold_binary, 255.0, cv::THRESH_BINARY);

  // Erode binary image
  img_binary = detector::ErodeAndDilate(img_binary, config_.erosion_size, config_.dilation_size);

  // Show images
	cv::imshow("rgb", img_rgb);
	cv::imshow("depth", img_depth);
	cv::imshow("diff", img_diff);
  cv::imshow("diff masked", img_diff_masked);
  cv::imshow("binary", img_binary);
  cv::waitKey(1);
}


bool PoseEstimation::Estimate_pose(pose_estimation::pose_est_srv::Request   &req,
		                pose_estimation::pose_est_srv::Response  &res)
{
  geometry_msgs::PoseArray posearray;

	std::vector<cv::Mat> object_points = Detect(img_rgb, img_depth, img_binary);
	posearray.header.stamp = ros::Time::now();

	for(int i = 0; i < object_points.size(); i++)
	{
		cv::Mat tmp_trans = object_points[i];
		float quat[4];
		getQuaternion(tmp_trans(cv::Rect(0,0,3,3)), quat);

		geometry_msgs::Pose p;
		p.position.x = tmp_trans.at<float>(0, 3);
		p.position.y = tmp_trans.at<float>(1, 3);
		p.position.z = tmp_trans.at<float>(2, 3);
    p.orientation.x = quat[0];
		p.orientation.y = quat[1];
		p.orientation.z = quat[2];
		p.orientation.w = quat[3];

		posearray.poses.push_back(p);
	}
	res.rel_object_poses = posearray;

  std::string out_str = "Number of detected objects: " + std::to_string(object_points.size());
	ROS_INFO_STREAM(out_str);
	return true;
}


std::vector<cv::Mat> PoseEstimation::Detect(cv::Mat &img_rgb, cv::Mat &img_depth, cv::Mat &img_binary)
{
  // Find contours
  std::vector<std::vector<cv::Point>> contours;
  findContours(img_binary, contours, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE, cv::Point(0, 0));
  cv::Mat filledContours = detector::FillContours(img_binary, contours);
  cv::imshow("filled", filledContours);

  // Find contours again after filling
  std::vector<std::vector<cv::Point>> contours2;
  findContours(filledContours, contours2, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE, cv::Point(0, 0));

  // Find median depth in contours
  std::vector<float> depth = detector::DepthWithinPerimeter(contours2, img_depth);

  // Find rotated rects
  std::vector<cv::RotatedRect> rot_rects = detector::FindRotatedRects(contours2);

  // Save detections to file
  detector::SaveToFile(img_rgb, img_depth, rot_rects);

  // Draw detections
  detector::ShowDetections(img_rgb, contours2, rot_rects);

  return detector::convert_2_transforms(rot_rects, depth, img_rgb.size().width, img_rgb.size().height, f_x, f_y, camera2base);
}


void PoseEstimation::getQuaternion(cv::Mat R, float Q[])
{
    float trace = R.at<float>(0,0) + R.at<float>(1,1) + R.at<float>(2,2);

    if (trace > 0.0)
    {
        float s = sqrt(trace + 1.0);
        Q[3] = (s * 0.5);
        s = 0.5 / s;
        Q[0] = ((R.at<float>(2,1) - R.at<float>(1,2)) * s);
        Q[1] = ((R.at<float>(0,2) - R.at<float>(2,0)) * s);
        Q[2] = ((R.at<float>(1,0) - R.at<float>(0,1)) * s);
    }

    else
    {
        int i = R.at<float>(0,0) < R.at<float>(1,1) ? (R.at<float>(1,1) < R.at<float>(2,2) ? 2 : 1) : (R.at<float>(0,0) < R.at<float>(2,2) ? 2 : 0);
        int j = (i + 1) % 3;
        int k = (i + 2) % 3;

        float s = sqrt(R.at<float>(i, i) - R.at<float>(j,j) - R.at<float>(k,k) + 1.0);
        Q[i] = s * 0.5;
        s = 0.5 / s;

        Q[3] = (R.at<float>(k,j) - R.at<float>(j,k)) * s;
        Q[j] = (R.at<float>(j,i) + R.at<float>(i,j)) * s;
        Q[k] = (R.at<float>(k,i) + R.at<float>(i,k)) * s;
    }
}


void PoseEstimation::OnDynamicReconfigure(PoseEstimation::DynamicReconfigureType &config, uint32_t level)
{
  (void)level;
  config_ = config;
  // Load background image
  img_empty_background = cv::imread(config_.empty_img_path);
  std::cout << "reconfig" << std::endl;

  // Subscribers

  bool resubscribe = !camera_sync_ ||
  subscriber_rgb_->getTopic() != config_.input_topic_rgb ||
  subscriber_depth_->getTopic() != config_.input_topic_depth;
  if (resubscribe)
  {
    subscriber_rgb_ = std::make_shared<decltype(subscriber_rgb_)::element_type>(nh_, config_.input_topic_rgb, 1);
    subscriber_depth_ = std::make_shared<decltype(subscriber_depth_)::element_type>(nh_, config_.input_topic_depth, 1);

    camera_sync_ = std::make_shared<CameraSynchronizer>(CameraSyncPolicy(1),
            *subscriber_rgb_,
            *subscriber_depth_
          );

  camera_sync_->registerCallback(boost::bind(&PoseEstimation::OnImage, this, _1, _2));


  }

  // Service
  if(!service_started)
  {
    service_ = nh_.advertiseService(config_.output_topic, &PoseEstimation::Estimate_pose, this);
    ROS_INFO("Ready to estimate relative position of the object.");
    service_started = true;
  }
}

