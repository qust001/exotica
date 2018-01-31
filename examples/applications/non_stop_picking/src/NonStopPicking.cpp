#include <non_stop_picking/NonStopPicking.h>
using namespace exotica;

NonStopPicking::NonStopPicking() : has_constraint_(false)
{
}

NonStopPicking::~NonStopPicking()
{
}

bool NonStopPicking::initialise(const std::string &rrtconnect_filepath, const std::string &endpose_filepath, const std::string &trajectory_filepath, const std::string &eef_link, unsigned int num_threads)
{
    num_threads_ = num_threads;
    Initializer solver, problem;

    rrtconnect_problems_.resize(num_threads_);
    rrtconnect_solvers_.resize(num_threads_);
    for (int i = 0; i < num_threads_; i++)
    {
        XMLLoader::load(rrtconnect_filepath, solver, problem);
        PlanningProblem_ptr any_problem = Setup::createProblem(problem);
        MotionSolver_ptr any_solver = Setup::createSolver(solver);
        rrtconnect_problems_[i] = std::static_pointer_cast<TimeIndexedSamplingProblem>(any_problem);
        rrtconnect_solvers_[i] = std::static_pointer_cast<TimeIndexedRRTConnect>(any_solver);
        rrtconnect_solvers_[i]->specifyProblem(rrtconnect_problems_[i]);
        rrtconnect_problems_[i]->getScene()->attachObject("Target", "Table");
    }

    XMLLoader::load(endpose_filepath, solver, problem);
    endpose_problem_ = std::static_pointer_cast<UnconstrainedEndPoseProblem>(Setup::createProblem(problem));
    endpose_solver_ = Setup::createSolver(solver);
    endpose_solver_->specifyProblem(endpose_problem_);

    XMLLoader::load(trajectory_filepath, solver, problem);
    trajectory_problem_ = std::static_pointer_cast<UnconstrainedTimeIndexedProblem>(Setup::createProblem(problem));
    trajectory_solver_ = Setup::createSolver(solver);
    trajectory_solver_->specifyProblem(trajectory_problem_);

    eef_link_ = eef_link;

    endpose_problem_->setRho("Position", 10000.0);
    endpose_problem_->setRho("Orientation", 5000.0);
    endpose_problem_->setRho("JointLimit", 5000.0);

    Eigen::VectorXd qs = rrtconnect_problems_[0]->applyStartState();
    rrtconnect_problems_[0]->getScene()->Update(qs, 0);
    default_target_pose_ = rrtconnect_problems_[0]->getScene()->getSolver().FK("Target", KDL::Frame(), "Table", KDL::Frame());

    rrtconnect_problems_[0]->getScene()->getCollisionScene()->setWorldLinkPadding(0.01);
    rrtconnect_problems_[0]->getScene()->updateCollisionObjects();

    for (int i = 0; i < trajectory_problem_->getT(); i++)
    {
        trajectory_problem_->setRho("Position", 10000.0, i);
        trajectory_problem_->setRho("Orientation", 5000.0, i);
        trajectory_problem_->setRho("JointLimit", 5000.0, i);
    }
}

bool NonStopPicking::setConstraint(Trajectory &cons, double start, double end)
{
    constraint_ = cons;
    tc_start_ = start;
    tc_end_ = end;
    has_constraint_ = true;
}

void NonStopPicking::solveConstraint(Eigen::VectorXd &q, double t)
{
    Eigen::MatrixXd solution;
    KDL::Frame y = constraint_.getPosition(t - tc_start_);
    endpose_problem_->setStartState(q);
    Eigen::Vector3d target_pos = Eigen::Vector3d(y.p.data[0], y.p.data[1], y.p.data[2]);
    endpose_problem_->setGoal("Position", target_pos);
    endpose_solver_->Solve(solution);
    q = solution.row(solution.rows() - 1);
}

void NonStopPicking::solveConstraintTrajectory(const Eigen::VectorXd &qs, double ta, double tb, Eigen::MatrixXd &solution)
{
    Eigen::MatrixXd tmp_solution;
    trajectory_problem_->setStartState(qs);
    // Initial Trajectory
    std::vector<Eigen::VectorXd> initial_guess;
    initial_guess.assign(trajectory_problem_->getT(), qs);
    trajectory_problem_->setInitialTrajectory(initial_guess);

    trajectory_solver_->Solve(tmp_solution);
    solution.resize(tmp_solution.rows(), tmp_solution.cols() + 1);
    double dt = (tb - ta) / (tmp_solution.rows() - 1.0);
    unsigned int i = 0;
    for (double t = ta; t < tb; t += dt)
    {
        solution(i, 0) = t;
        solution.row(i).tail(tmp_solution.cols()) = tmp_solution.row(i);
        i++;
    }
}

void NonStopPicking::publishTrajectory(const Eigen::MatrixXd &solution)
{
    rrtconnect_problems_[0]->getScene()->attachObjectLocal("Target", "Table", default_target_pose_);
    bool first = true;

    for (int i = 0; i < solution.rows() - 1; i++)
    {
        double t = solution(i, 0);
        Eigen::VectorXd q = solution.row(i).tail(solution.cols() - 1);
        rrtconnect_problems_[0]->getScene()->Update(q, t);

        if (first && t >= tc_end_)
        {
            first = false;
            rrtconnect_problems_[0]->getScene()->attachObject("Target", eef_link_);
        }
        rrtconnect_problems_[0]->getScene()->publishScene();
        rrtconnect_problems_[0]->getScene()->getSolver().publishFrames();
        ros::Duration(solution(i + 1, 0) - t).sleep();
    }
}

bool NonStopPicking::solve(const CTState &start, const CTState &goal, Eigen::MatrixXd &solution)
{
    ros::Time start_time = ros::Time::now();
    Eigen::MatrixXd solution_pre, solution_after, solution_constrained;
    Eigen::VectorXd q_start = rrtconnect_problems_[0]->applyStartState();
    Eigen::VectorXd q_goal = rrtconnect_problems_[0]->getGoalState();
    double t_goal = rrtconnect_problems_[0]->getGoalTime();
    Eigen::VectorXd qs = q_start;

    solveConstraint(qs, tc_start_);
    solveConstraintTrajectory(qs, tc_start_, tc_end_, solution_constrained);
    Eigen::VectorXd qa = solution_constrained.row(0).tail(q_goal.size()), qb = solution_constrained.row(solution_constrained.rows() - 1).tail(q_goal.size());

    HIGHLIGHT("Optimization time " << ros::Duration(ros::Time::now() - start_time).toSec());

    std::shared_ptr<ompl::base::PlannerTerminationCondition> ptc;
    ptc.reset(new ompl::base::PlannerTerminationCondition(ompl::base::timedPlannerTerminationCondition(5.0)));
    for (int i = 0; i < rrtconnect_problems_.size(); i++)
        rrtconnect_solvers_[i]->setPlannerTerminationCondition(ptc);

    start_time = ros::Time::now();
    solveRRTConnectMultiThreads(q_start, qa, 0, tc_start_, solution_pre);

    for (int i = 0; i < rrtconnect_problems_.size(); i++)
    {
        rrtconnect_problems_[i]->getScene()->Update(qb, tc_end_);
        rrtconnect_problems_[i]->getScene()->attachObject("Target", eef_link_);
    }

    ptc.reset(new ompl::base::PlannerTerminationCondition(ompl::base::timedPlannerTerminationCondition(5.0)));
    for (int i = 0; i < rrtconnect_problems_.size(); i++)
        rrtconnect_solvers_[i]->setPlannerTerminationCondition(ptc);
    solveRRTConnectMultiThreads(qb, q_goal, tc_end_, t_goal, solution_after);

    solution.resize(solution_pre.rows() + solution_constrained.rows() - 2 + solution_after.rows(), solution_pre.cols());
    solution << solution_pre,
        solution_constrained.block(1, 0, solution_constrained.rows() - 2, solution_constrained.cols()),
        solution_after;

    HIGHLIGHT("RRT-Connect time " << ros::Duration(ros::Time::now() - start_time).toSec());

    // HIGHLIGHT("Solution \n"<<solution);
    return true;
}

void NonStopPicking::solveRRTConnectMultiThreads(const Eigen::VectorXd &qa, const Eigen::VectorXd &qb, double ta, double tb, Eigen::MatrixXd &solution)
{
    std::vector<boost::thread *> th(num_threads_);
    for (unsigned int i = 0; i < num_threads_; i++)
        th[i] = new boost::thread(
            boost::bind(&NonStopPicking::solveRRTConnect, this, qa, qb, ta, tb, i));
    for (unsigned int i = 0; i < num_threads_; ++i)
    {
        th[i]->join();
        delete th[i];
    }

    solution = rrtconnect_solution_;
}

void NonStopPicking::solveRRTConnect(const Eigen::VectorXd qa, const Eigen::VectorXd qb, double ta, double tb, unsigned int tid)
{
    rrtconnect_problems_[tid]->setStartState(qa);
    rrtconnect_problems_[tid]->setStartTime(ta);
    rrtconnect_problems_[tid]->setGoalState(qb);
    rrtconnect_problems_[tid]->setGoalTime(tb);
    rrtconnect_solvers_[tid]->Solve(rrtconnect_solution_);
}