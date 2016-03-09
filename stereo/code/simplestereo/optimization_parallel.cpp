//
// Created by yanhang on 3/8/16.
//
#include "optimization.h"
#include "../external/QPBO1.4/QPBO.h"

using namespace std;
using namespace cv;
using namespace ParallelFusion;

namespace simple_stereo {

    double ParallelOptimize::optimize(stereo_base::Depth &result, const int max_iter) const {
        typedef CompactLabelSpace Space;
        typedef ParallelFusionPipeline<Space> Pipeline;

        result.initialize(width, height, -1);
        //configure as sequential fusion
        ParallelFusionOption pipelineOption;
        pipelineOption.num_threads = 7;
        pipelineOption.max_iteration = 1;
        const int kLabelPerThread = model.nLabel / pipelineOption.num_threads;

        Pipeline::GeneratorSet generators((size_t)pipelineOption.num_threads);
        Pipeline::SolverSet solvers((size_t)pipelineOption.num_threads);
        vector<Space> initials((size_t)pipelineOption.num_threads);
        vector<ThreadOption> threadOptions((size_t)pipelineOption.num_threads);
        for(auto& to: threadOptions){
            to.kOtherThread = 0;
            to.kSelfThread = 40;
        }
        const int kPix = model.width * model.height;

        for(auto i=0; i<pipelineOption.num_threads; ++i){
            const int startid = i * kLabelPerThread;
            const int endid = (i+1) * kLabelPerThread - 1;

            initials[i].init(kPix, vector<int>(1, startid));
            printf("Thread %d, start: %d, end: %d\n", i, startid, endid);
            generators[i] = shared_ptr<ProposalGenerator<Space> >(new SimpleStereoGenerator(model.width * model.height, startid, endid));
            solvers[i] = shared_ptr<FusionSolver<Space> >(new SimpleStereoSolver(&model));
            printf("Initial energy on thread %d: %.5f\n", i, solvers[i]->evaluateEnergy(initials[i]));
        }

        Pipeline parallelFusionPipeline(pipelineOption);

        float t = cv::getTickCount();
        printf("Start runing parallel optimization\n");
        parallelFusionPipeline.runParallelFusion(initials, generators, solvers, threadOptions);
        t = ((float)getTickCount() - t) / (float)getTickFrequency();

        SolutionType<Space > solution;
        parallelFusionPipeline.getBestLabeling(solution);
        printf("Done! Final energy: %.5f, running time: %.3f\n", solution.first, t);

        for(auto i=0; i<model.width * model.height; ++i){
            result.setDepthAtInd(i, solution.second(i,0));
        }
        return solution.first;
    }


    SimpleStereoGenerator::SimpleStereoGenerator(const int nPix_, const int startid_, const int endid_, const bool randomOrder_):
            nPix(nPix_), startLabel(startid_), endLabel(endid_), randomOrder(randomOrder_), nextLabel(0){
        labelTable.resize((size_t)(endLabel - startLabel + 1));
        for(auto i=0; i<labelTable.size(); ++i)
            labelTable[i] = i + startLabel;
        if(randomOrder)
            std::random_shuffle(labelTable.begin(), labelTable.end());
    }

    void SimpleStereoGenerator::getProposals(CompactLabelSpace &proposals,
                                            const CompactLabelSpace &current_solution, const int N) {
        for(auto i=0; i<N; ++i){
            proposals.getSingleLabel().push_back(labelTable[nextLabel]);
            nextLabel = (nextLabel + 1) % (int)labelTable.size();
        }
    }

    void SimpleStereoSolver::initSolver(const CompactLabelSpace &initial) {
        CHECK_EQ(initial.getNumNode(), model->width * model->height);
        EnergyFunction *energy_function = new EnergyFunction(new DataCost(const_cast<int*>(model->MRF_data.data())),
                                                             new SmoothnessCost(1, 4, model->weight_smooth,
                                                                                const_cast<int*>(model->hCue.data()),
                                                                                const_cast<int*>(model->vCue.data())));
        mrf = shared_ptr<Expansion>(new Expansion(model->width, model->height, model->nLabel, energy_function));
        mrf->initialize();
    }

    void SimpleStereoSolver::solve(const CompactLabelSpace &proposals,
                                     const SolutionType<CompactLabelSpace>& current_solution,
                                     SolutionType<CompactLabelSpace>& solution){
        CHECK(!proposals.empty());
        int kFullProposal;
        if(proposals.getLabelSpace().empty())
            kFullProposal = 0;
        else
            kFullProposal = (int) proposals.getLabelOfNode(0).size();

        const vector<int> &singleLabel = proposals.getSingleLabel();

        solution = current_solution;
        for (auto i = 0; i < singleLabel.size(); ++i) {
            printf("Fusing proposal with graph cut %d\n", singleLabel[i] );
            for(auto i=0; i<kPix; ++i) {
                mrf->setLabel(i, solution.second(i, 0));
            }
            mrf->alpha_expansion(singleLabel[i]);
            for(auto i=0; i<kPix; ++i)
                solution.second(i,0) = mrf->getLabel(i);
            cout << "done" << endl << flush;
        }
        for (auto i = 0; i < kFullProposal; ++i) {
            //run QPBO
            printf("Running QPBO...\n");
            kolmogorov::qpbo::QPBO<int> qpbo(kPix, 4 * kPix);
            qpbo.AddNode(kPix);
            for (auto j = 0; j < kPix; ++j)
                qpbo.AddUnaryTerm(j, model->operator()(j, current_solution.second(i,0)), model->operator()(j, proposals(j, i)));

            for (auto y = 0; y < model->height - 1; ++y) {
                for (auto x = 0; x < model->width - 1; ++x) {
                    int e00, e01, e10, e11;
                    int pix1 = y * model->width + x, pix2 = y * model->width + x + 1, pix3 =
                            (y + 1) * model->width + x;
                    //x direction
                    e00 = smoothnessCost(pix1, solution.second(pix1, 0), solution.second(pix2, 0), true);
                    e01 = smoothnessCost(pix1, solution.second(pix1, 0), proposals(pix2, i), true);
                    e10 = smoothnessCost(pix1, proposals(pix1, i), solution.second(pix2, 0), true);
                    e11 = smoothnessCost(pix1, proposals(pix1, i), proposals(pix2, i), true);
                    qpbo.AddPairwiseTerm(pix1, pix2, e00, e01, e10, e11);

                    //y direction
                    e00 = smoothnessCost(pix1, solution.second(pix1, 0), solution.second(pix3, 0), false);
                    e01 = smoothnessCost(pix1, solution.second(pix1, 0), proposals(pix3, i), false);
                    e10 = smoothnessCost(pix1, proposals(pix1, i), solution.second(pix3, 0), false);
                    e11 = smoothnessCost(pix1, proposals(pix1, i), proposals(pix3, i), false);
                    qpbo.AddPairwiseTerm(pix1, pix3, e00, e01, e10, e11);
                }
            }

            qpbo.Solve();
            qpbo.ComputeWeakPersistencies();

            for (auto pix = 0; pix < kPix; ++pix) {
                if (qpbo.GetLabel(pix) > 0)
                    solution.second(pix, 0) = proposals(pix, i);
            }
        }

        //solution.first = evaluateEnergy(solution.second);
        solution.first = evaluateEnergy(solution.second);
        cout << "Current energy: " << solution.first << endl << flush;
    }

    double SimpleStereoSolver::evaluateEnergy(const CompactLabelSpace &solution) const {
        CHECK_EQ(solution.getNumNode(), kPix);
        double e = 0;
        const int w = model->width;
        const int h = model->height;
        const double r = model->MRFRatio;
        for(auto i=0; i<kPix; ++i) {
            e += model->MRF_data[i * model->nLabel + solution(i, 0)] / model->MRFRatio;
        }
        for(auto x=0; x<w - 1; ++x) {
            for (auto y = 0; y < h - 1; ++y) {
                int sc = smoothnessCost(y * w + x, solution(y * w + x, 0), solution(y * w + x + 1, 0), true) +
                         smoothnessCost(y * w + x, solution(y * w + x, 0), solution((y + 1) * w + x, 0), true);
                e += (double) sc / r;
            }
        }
        return e;
    }
}