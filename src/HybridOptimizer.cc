// -*- lsst-c++ -*-
/* 
 * LSST Data Management System
 * Copyright 2012 LSST Corporation.
 * 
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the LSST License Statement and 
 * the GNU General Public License along with this program.  If not, 
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

#include "Eigen/Core"
#include "Eigen/Eigenvalues"
#include "Eigen/Cholesky"
#include "boost/make_shared.hpp"

#include "lsst/meas/extensions/multiShapelet/HybridOptimizer.h"

namespace lsst { namespace meas { namespace extensions { namespace multiShapelet {

// Throughout this file, I have used the variable names from the original implementation
// and/or the formulae in the document it is based on (see class docs), rather than
// names that adhere to the LSST naming conventions (which are follwed in the header file).
// No amount of longer, more descriptive names would make this code understandable without
// a mathematical understanding of the algorithm, so I think it's more important to use
// variable names that can easily be mapped to variables in a more complete description.

class HybridOptimizer::Impl {
public:
    
    Impl(
        PTR(Objective) const & objective,
        ndarray::Array<double const,1,1> const & parameters,
        Control const & control
    );

    void step();

    void solve(Eigen::MatrixXd const & m);

    bool checkStep(double stepNorm, StateFlags bad) {
        if (!(stepNorm > ctrl.minStep * (x.norm() + ctrl.minStep))) {
            state |= bad;
            return false;
        }
        return true;
    }

    PTR(Objective) obj;
    HybridOptimizerControl ctrl;
    MethodEnum method;
    int state;
    int count;
    int rank;
    ndarray::EigenView<double,1,1> x;
    ndarray::EigenView<double,1,1> xNew;
    ndarray::EigenView<double,1,1> f;
    ndarray::EigenView<double,1,1> fNew;
    ndarray::EigenView<double,2,-2> J;
    ndarray::EigenView<double,2,-2> JNew;
    Eigen::VectorXd h;
    Eigen::VectorXd y;
    Eigen::VectorXd v;
    Eigen::MatrixXd A; // Hessian for LM method
    Eigen::MatrixXd B; // Hessian for BFGS method (Jarvis uses 'H')
    Eigen::VectorXd g;
    Eigen::VectorXd gNew;
    Eigen::LDLT<Eigen::MatrixXd,Eigen::Lower> ldlt;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigh;
    double normInfF;
    double normInfG;
    double Q;
    double QNew;
    double mu;
    double nu;
    double delta;
};

HybridOptimizer::Impl::Impl(
    PTR(Objective) const & objective,
    ndarray::Array<double const,1,1> const & parameters,
    Control const & control
) : obj(objective), ctrl(control), method(LM), state(0), count(0),
    rank(objective->getParameterSize()),
    x(ndarray::copy(parameters)), xNew(ndarray::copy(parameters)),
    f(ndarray::allocate(objective->getFunctionSize())),
    fNew(ndarray::allocate(objective->getFunctionSize())),
    J(ndarray::allocate(objective->getFunctionSize(), objective->getParameterSize())),
    JNew(ndarray::allocate(objective->getFunctionSize(), objective->getParameterSize())),
    h(Eigen::VectorXd::Zero(objective->getParameterSize())),
    y(Eigen::VectorXd::Zero(objective->getParameterSize())),
    v(Eigen::VectorXd::Zero(objective->getParameterSize())),
    A(Eigen::MatrixXd::Zero(objective->getParameterSize(), objective->getParameterSize())),
    B(Eigen::MatrixXd::Identity(objective->getParameterSize(), objective->getParameterSize())),
    g(Eigen::VectorXd::Zero(objective->getParameterSize())),
    gNew(Eigen::VectorXd::Zero(objective->getParameterSize())),
    ldlt(0), eigh(0), normInfF(0.0), normInfG(0.0), Q(0.0), QNew(0.0), mu(0.0), nu(2.0), delta(ctrl.delta0)
{
    fNew.setZero();
    obj->computeFunction(xNew.shallow(), fNew.shallow());
    f = fNew;
    normInfF = f.lpNorm<Eigen::Infinity>();
    QNew = Q = 0.5 * f.squaredNorm();
    JNew.setZero(); 
    obj->computeDerivative(xNew.shallow(), fNew.shallow(), JNew.shallow());
    J = JNew;
    A.selfadjointView<Eigen::Lower>().rankUpdate(J.adjoint());
    g = J.adjoint() * f;
    normInfG = g.lpNorm<Eigen::Infinity>();
    mu = ctrl.tau * A.diagonal().lpNorm<Eigen::Infinity>();
    A.diagonal().array() += mu;
}

void HybridOptimizer::Impl::step() {
    static double const sqrtEps = std::sqrt(std::numeric_limits<double>::epsilon());
    bool isBetter = false;
    bool shouldSwitchMethod = false;

    switch (method) {
    case LM:
        solve(A);
        break;
    case BFGS:
        solve(B);
        break;
    }
    
    double normH = h.norm();
    if (!checkStep(normH, FAILURE_MINSTEP)) return;
    if (method == BFGS && normH > delta) h *= delta / normH;
    xNew = x + h;
    // All of the doStep business is a very kludgy way to add limited constraints to the optimizer;
    // it does not make this a general robust constrained optimizer.  But hopefully it's enough for
    // some simple problems, like galaxy models with tiny radii.
    Objective::StepResult doStep = obj->tryStep(x.shallow(), xNew.shallow());
    if (doStep == Objective::MODIFIED) {
        // The objective function modified our proposal step into something it could evaluate.
        // We'll proceed as this was the step we proposed, but first we'll check if we're
        // actually going anywhere.
        state |= STEP_MODIFIED;
        h = xNew - x;
        normH = h.norm();
        if (!checkStep(normH, FAILURE_MINSTEP)) return;
    } else if (doStep == Objective::INVALID) {
        // Proposed step is so ridiculuous we won't even evaluate the model, but we will
        // modify the trust region parameters (delta or mu,nu), so we don't return just yet.
        // This means we can't update the BFGS Hessian approximation during this step, so
        // it doesn't help us as much as an evaluated-and-rejected step might - but it's
        // also a lot cheaper.
        state |= STEP_INVALID;
        QNew = std::numeric_limits<double>::infinity();
    } else {
        state &= ~(STEP_MODIFIED | STEP_INVALID);
    }
    if (doStep) {
        fNew.setZero();
        obj->computeFunction(xNew.shallow(), fNew.shallow());
        QNew = 0.5 * fNew.squaredNorm();
        JNew.setZero();
        obj->computeDerivative(xNew.shallow(), fNew.shallow(), JNew.shallow());
    }

    double normInfGNew = 0.0;
    if (doStep && (method == BFGS || QNew < Q)) {
        gNew = JNew.adjoint() * fNew;
        normInfGNew = gNew.lpNorm<Eigen::Infinity>();
    }

    if (method == BFGS) {
        isBetter = (QNew < Q) || (QNew <= (1.0 + sqrtEps) * Q && normInfGNew < normInfG);
        shouldSwitchMethod = (normInfGNew >= normInfG);
        if (QNew < Q) {
            double rho = (Q - QNew) / -(h.dot(g) - 0.5*(J*h).squaredNorm());
            if (rho > 0.75) {
                delta = std::max(delta, 3.0 * normH);
            } else if (rho < 0.25) {
                delta /= 2.0;
                if (!checkStep(delta, FAILURE_MINTRUST)) return;
            }
        } else {
            delta /= 2.0;
            if (!checkStep(delta, FAILURE_MINTRUST)) return;
        }
    } else { // method == LM
        if (QNew < Q) {
            isBetter = true;
            double rho = (Q - QNew) / (-0.5 * h.dot(g - mu * h));
            mu *= std::max(1.0 / 3.0, 1.0 - std::pow(2.0 * rho - 1.0, 3));
            nu = 2.0;
            if (std::min(normInfGNew, Q - QNew) < 0.02 * QNew) {
                if (++count == 3) shouldSwitchMethod = true;
            } else {
                count = 0;
            }
            if (count != 3) {
                A.setZero();
                A.selfadjointView<Eigen::Lower>().rankUpdate(JNew.adjoint());
                A.diagonal().array() += mu;
            }
        } else {
            A.diagonal().array() += mu * (nu - 1.0);
            mu *= nu;
            nu *= 2.0;
            shouldSwitchMethod = (nu >= 32.0);
        }
    }
    if (!doStep) return;

    y = JNew.adjoint() * (JNew * h) + (gNew - g);
    double hy = h.dot(y);
    if (hy > 0.0) {
        v = B.selfadjointView<Eigen::Lower>() * h;
        double hv = h.dot(v);
        B.selfadjointView<Eigen::Lower>().rankUpdate(v, -1.0 / hv);
        B.selfadjointView<Eigen::Lower>().rankUpdate(y, 1.0 / hy);
    }

    if (isBetter) {
        x = xNew;
        f = fNew;
        Q = QNew;
        J = JNew;
        g = gNew;
        normInfF = f.lpNorm<Eigen::Infinity>();
        normInfG = normInfGNew;
        if (!(normInfF > ctrl.fTol)) {
            state |= SUCCESS_FTOL;
        }
        if (!(normInfG > ctrl.gTol)) {
            state |= SUCCESS_GTOL;
        }
    }

    if (shouldSwitchMethod) {
        if (method == BFGS) { // switching from BFGS to LM
            A.setZero();
            A.selfadjointView<Eigen::Lower>().rankUpdate(J.adjoint());
            A.diagonal().array() += mu;
            method = LM;
        } else { // switching from LM to BFGS
            delta = std::max(1.5 * ctrl.minStep * (f.squaredNorm() + ctrl.minStep), 0.2 * normH);
            method = BFGS;
        }
    }

    if (isBetter) {
        state |= STEP_ACCEPTED;
    } else {
        state &= ~STEP_ACCEPTED;
    }
}

void HybridOptimizer::Impl::solve(Eigen::MatrixXd const & m) {
    if (ctrl.useCholesky) {
        ldlt.compute(m);
        h = ldlt.solve(-g);
    } else {
        rank = obj->getParameterSize();
        eigh.compute(m);
        double threshold = eigh.eigenvalues()[rank-1] * std::numeric_limits<double>::epsilon();
        int i = 0;
        for (; i < rank && eigh.eigenvalues()[i] < threshold; ++i);
        rank -= i;
        h = eigh.eigenvectors().rightCols(rank) *
            eigh.eigenvalues().tail(rank).array().inverse().matrix().asDiagonal() * 
            eigh.eigenvectors().rightCols(rank).adjoint() * (-g);
    }
}

int HybridOptimizer::step() {
    _impl->step();
    return _impl->state;
}

int HybridOptimizer::getState() const {
    return _impl->state;
}

int HybridOptimizer::run() {
    for (int n = 0; n < _impl->ctrl.maxIter; ++n) {
        _impl->step();
        if (_impl->state & FINISHED) return _impl->state;
    }
    _impl->state |= FAILURE_MAXITER;
    return _impl->state;
}

HybridOptimizer::MethodEnum HybridOptimizer::getMethod() const { return _impl->method; }
double HybridOptimizer::getChiSq() const { return 2.0 * _impl->Q; }
double HybridOptimizer::getTrialChiSq() const { return 2.0 * _impl->QNew; }
double HybridOptimizer::getFunctionInfNorm() const { return _impl->normInfF; }
double HybridOptimizer::getGradientInfNorm() const { return _impl->normInfG; }
double HybridOptimizer::getMu() const { return _impl->mu; }
double HybridOptimizer::getDelta() const { return _impl->delta; }

CONST_PTR(Objective) HybridOptimizer::getObjective() const { return _impl->obj; }

ndarray::Array<double const,1,1> HybridOptimizer::getParameters() const { return _impl->x.shallow(); }
ndarray::Array<double const,1,1> HybridOptimizer::getTrialParameters() const { return _impl->xNew.shallow(); }
ndarray::Array<double const,1,1> HybridOptimizer::getFunction() const { return _impl->f.shallow(); }
ndarray::Array<double const,1,1> HybridOptimizer::getTrialFunction() const { return _impl->fNew.shallow(); }

HybridOptimizerControl const & HybridOptimizer::getControl() const { return _impl->ctrl; }

HybridOptimizer::HybridOptimizer(
    PTR(Objective) const & objective,
    ndarray::Array<double const,1,1> const & parameters,
    Control const & ctrl
) : _impl(boost::make_shared<Impl>(objective, parameters, ctrl))
{}

HybridOptimizer::~HybridOptimizer() {}

}}}} // namespace lsst::meas::extensions::multiShapelet
