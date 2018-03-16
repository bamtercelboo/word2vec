/**
* @Author: bamtercelboo
* @Date: 2018/03/15
* @File: fasttext.h
* @Contact: bamtercelboo@{gmail.com, 163.com}
* @Function: None
*/


#pragma once

#include<time.h>

#include<iostream>
#include<vector>
#include<string>
#include<memory>
#include<atomic>
#include<iomanip>
#include <thread>

#include "args.h"
#include "dictionary.h"
#include "matrix.h"
#include "model.h"
#include "real.h"
#include "utils.h"

class FastText {
  protected:
	std::shared_ptr<Args> args_;
	std::shared_ptr<Dictionary> dict_;

	std::shared_ptr<Matrix> input_;
	std::shared_ptr<Matrix> output_;

	std::shared_ptr<Model> model_;

	std::atomic<int64_t> tokenCount_;
	std::atomic<real> loss_;

	clock_t start_;

	void startThreads();

  public:
	FastText();
	void saveVectors();
	void printInfo(real, real, std::ostream&);

	void skipgram(Model&, real, const std::vector<std::vector<int32_t> >&, const std::vector<int32_t>&);
	void trainThread(int32_t);
	void train(const Args);
};

FastText::FastText() {}

void FastText::train(const Args args) {
	args_ = std::make_shared<Args>(args);
	dict_ = std::make_shared<Dictionary>(args_);
	if (args_->input == "-") {
		//manage expectations
		throw std::invalid_argument("Cannot use  stdin for training");
	}
	std::ifstream ifs(args_->input);
	if (!ifs.is_open()) {
		throw std::invalid_argument(args_->input + "cannot be opened for training!");
	}

	// read file to dict
	dict_->readFromFile(ifs);
	ifs.close();

	input_ = std::make_shared<Matrix>(dict_->nwords(), args_->dim);
	input_->uniform(1.0 / args_->dim);

	output_ = std::make_shared<Matrix>(dict_->nwords(), args_->dim);
	output_->zero();
	startThreads();
	model_ = std::make_shared<Model>(input_, output_, args_, 0);
	model_->setTargetCounts(dict_->getCounts());
}


void FastText::printInfo(real progress, real loss, std::ostream& log_stream) {
	// clock_t might also only be 32bits wide on some systems
	double t = double(clock() - start_) / double(CLOCKS_PER_SEC);
	double lr = args_->lr * (1.0 - progress);
	double wst = 0;
	int64_t eta = 720 * 3600; // Default to one month
	if (progress > 0 && t >= 0) {
		eta = int(t / progress * (1 - progress) / args_->thread);
		wst = double(tokenCount_) / t;
	}
	int64_t etam = (eta % 3600) / 60;
	int64_t etah = etam / 60;
	progress = progress * 100;
	log_stream << std::fixed;
	log_stream << "Progress: ";
	log_stream << std::setprecision(1) << std::setw(5) << progress << "%";
	log_stream << " words/sec/thread: " << std::setw(7) << int64_t(wst);
	log_stream << " lr: " << std::setw(9) << std::setprecision(6) << lr;
	log_stream << " loss: " << std::setw(9) << std::setprecision(6) << loss;
	//log_stream << " ETA: " << std::setw(3) << etah;
	//log_stream << "h" << std::setw(2) << etam << "m";
	log_stream << std::flush;
}

void FastText::skipgram(Model& model, real lr, const std::vector<std::vector<int32_t> >& source,
	const std::vector<int32_t>& target) {
	std::uniform_int_distribution<> uniform(1, args_->ws);
	for (int32_t w = 0; w < target.size(); w++) {
		int32_t boundary = uniform(model.rng);
		const std::vector<int32_t>& ngrams = source[w];
		//std::cout << ngrams[0] << std::endl;
		assert(ngrams.size() == 1);
		for (int32_t c = -boundary; c <= boundary; c++) {
			if (c != 0 && w + c >= 0 && w + c < target.size()) {
				model.update(ngrams, target[w + c], lr);
			}
		}
	}
}

void FastText::trainThread(int32_t threadId) {
	std::ifstream ifs(args_->input);
	utils::seek(ifs, threadId * utils::size(ifs) / args_->thread);

	Model model(input_, output_, args_, threadId);
	model.setTargetCounts(dict_->getCounts());

	const int64_t ntokens = dict_->ntokens();
	int64_t localTokenCount = 0;
	std::vector<std::vector<int32_t> > sourceType;
	std::vector<std::vector<int32_t> > source;
	std::vector<int32_t> target;
	while (tokenCount_ < args_->epoch * ntokens) {
		real process = real(tokenCount_) / (args_->epoch * ntokens);
		real lr = args_->lr * (1.0 - process);
		if (lr < 0.0001 * args_->lr)
			lr = 0.0001 * args_->lr;
		if (args_->model == model_name::skipgram) {
			localTokenCount += dict_->getLine(ifs, sourceType, source, target, model.rng);
			//std::cout << localTokenCount << std::endl;
			skipgram(model, lr, source, target);
		}
		if (localTokenCount > args_->lrUpdateRate) {
			tokenCount_ += localTokenCount;
			localTokenCount = 0;
			if (threadId == 0 && args_->verbose > 1)
				loss_ = model.getLoss();
		}
	}
	if (threadId == 0)
		loss_ = model.getLoss();
	ifs.close();
}

void FastText::startThreads() {
	start_ = clock();
	tokenCount_ = 0;
	loss_ = -1;
	std::vector<std::thread> threads;
	for (int32_t i = 0; i < args_->thread; i++) {
		threads.push_back(std::thread([=]() {
			trainThread(i);
		}));
	}
	const int64_t ntokens = dict_->ntokens();
	// Same condition as trainThread
	while (tokenCount_ < args_->epoch * ntokens) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (loss_ >= 0 && args_->verbose > 1) {
			real progress = real(tokenCount_) / (args_->epoch * ntokens);
			std::cerr << "\r";
			printInfo(progress, loss_, std::cerr);
		}
	}
	for (int32_t i = 0; i < args_->thread; i++) {
		threads[i].join();
	}
	if (args_->verbose > 0) {
		std::cerr << "\r";
		printInfo(1.0, loss_, std::cerr);
		std::cerr << std::endl;
	}
}

void FastText::saveVectors() {
	int32_t nwords = dict_->nwords();
	int32_t ntargets = dict_->ntargets();

	Vector vec(args_->dim);

	if (nwords > 0) {
		std::ofstream ofs(args_->output + ".source");
		if (!ofs.is_open()) {
			throw std::invalid_argument(args_->output + ".source" + " cannot be opened for saving source embedding.");
		}
		for (int32_t i = 0; i < nwords; i++){
			std::string word = dict_->getWord(i);
			vec.zero();
			vec.addRow(*input_, i);
			ofs << word << " " << vec << std::endl;
		}
		ofs.close();
	}

	if (ntargets > 0) {
		std::ofstream ofs(args_->output + ".target");
		if (!ofs.is_open()) {
			throw std::invalid_argument(args_->output + ".target" + " cannot be opened for saving target embedding.");
		}
		for (int32_t i = 0; i < ntargets; i++) {
			std::string word = dict_->getTarget(i);
			vec.zero();
			vec.addRow(*output_, i);
			ofs << word << " " << vec << std::endl;
		}
		ofs.close();
	}
}








