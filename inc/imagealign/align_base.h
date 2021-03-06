/**
 This file is part of Image Alignment.
 
 Copyright Christoph Heindl 2015
 
 Image Alignment is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 Image Alignment is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with Image Alignment.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef IMAGE_ALIGN_ALIGN_BASE_H
#define IMAGE_ALIGN_ALIGN_BASE_H

#include <imagealign/warp.h>
#include <imagealign/config.h>
#include <imagealign/image_pyramid.h>

#include <limits>


namespace imagealign {
    
    template<class W>
    struct SingleStepResult {
        typename W::Traits::ParamType delta;
        typename W::Traits::ScalarType sumErrors;
        int numConstraints;
        
        SingleStepResult()
         : numConstraints(0)
        {}
    };
   
    /**
        Base class for alignment algorithms.
     
        This class provides a common interface for alignment algorithms. It simplifies code
        in derived classes through
            - preparing multi-level image pyramid for hierarchical matching
            - common alignment interface
            - providing access to alignment information such as error, error change and more.

        ## Multi-level image alignment
     
        When doing multi-level image alignment one needs to consider how to generate the coarser
        pyramid levels and how to perform matching on them. 
     
        The coarser pyramid levels are generated by halfing the width and height of the parent level 
        and linearly interpolated the new pixels from the parent level. As far as matching on coarse
        levels is concerned one either has the option to scale down the warp function or leave the
        warp function operating on the original level and scale image coordinates. As this library
        does not assume warp functions can be scaled in general, we rather scale image coordintes before
        dealing with the warp. This effectively means that the warp always operates on the finest pyramid
        level.
     */
    template<class D, class W>
    class AlignBase {
    public:
        
        typedef AlignBase<D, W> SelfType;
        typedef typename W::Traits::ScalarType ScalarType;
        
        /** 
            Prepare for alignment.
         
            This function takes the template and target image and performs
            necessary pre-calculations to speed up the alignment process.
         
            \param tmpl Single channel template image
            \param target Single channel target image to align template with.
            \param w The warp.
            \param pyramidLevels Maximum number of pyramid levels to generate.
         */
        void prepare(cv::InputArray tmpl, cv::InputArray target, const W &w, int pyramidLevels)
        {
            // Do the basic thing everyone needs
            CV_Assert(tmpl.channels() == 1);
            CV_Assert(target.channels() == 1);
            
            // Sanitize levels
            int maxLevels = std::min<int>(ImagePyramid::maxLevelsForImageSize(tmpl.size()),
                                          ImagePyramid::maxLevelsForImageSize(target.size()));
            
            _levels = std::max<int>(1, std::min<int>(pyramidLevels, maxLevels));
            
            _templatePyramid.create(tmpl, _levels);
            _targetPyramid.create(target, _levels);            
            
            setLevel(0);
            
            // Invoke prepare of derived
            static_cast<D*>(this)->prepareImpl(w);
        }
        
        /**
            Prepare for alignment.
         
            This function takes the template image and an pre built target image pyramid
            and performs necessary pre-calculations to speed up the alignment process.
         
            This function comes in handy when you want to track multiple templates on the same
            target image. Then, the target image pyramid can be built once, and shared among all
            alignment objects.
         
            \param tmpl Single channel template image
            \param target Pre-built image pyramid of target image.
            \param w The warp.
            \param pyramidLevels Maximum number of pyramid levels to generate.
         */
        void prepare(cv::InputArray tmpl, const ImagePyramid &target, const W &w, int pyramidLevels)
        {
            // Do the basic thing everyone needs
            CV_Assert(target.numLevels() > 0);
            CV_Assert(tmpl.channels() == 1);
            CV_Assert(target[0].channels() == 1);
            
            
            // Sanitize levels
            int maxLevels = std::min<int>(ImagePyramid::maxLevelsForImageSize(tmpl.size()),
                                          target.numLevels());

            _levels = std::max<int>(1, std::min<int>(pyramidLevels, maxLevels));
            _templatePyramid.create(tmpl, _levels);

            if (target.numLevels() > _levels) {
                _targetPyramid = target.slice(0, _levels);
            } else {
                _targetPyramid = target;
            }
            
            setLevel(0);
            
            // Invoke prepare of derived
            static_cast<D*>(this)->prepareImpl(w);
        }
        
        /**
            Perform multiple alignment iterations until a stopping criterium is reached.
         
            This method takes the current state of the warp parameters and refines
            them by minimizing the energy function of the derived class.
         
            Iterations are performed on all levels of the pyramid. The algorithm starts at the 
            coarsest pyramid and iterates a stopping criterium of the current level is matched. 
            Once a stopping criterium is met, the algorithm breaks to the next finer pyramid level.

            Currently the iteration is stopped when
                - the number of iterations exceeds the number of iterations per level.
                - the length of delta parameter vector estimated is less than eps
                - an increase of error is observed (with exception between two pyramid layers)
         
            \param w Current state of warp estimation. Will be modified to hold result.
            \param maxIterations Maximum number of iterations in all levels.
            \param eps Minimum length of incremental parameter vector to continue on current level.
            \param steps Optional container to receiver intermediate steps for debugging purposes.
         */
        SelfType &align(W &w, int maxIterations, ScalarType eps, std::vector<W> *steps = 0)
        {
            int iterationsPerLevel = maxIterations / numLevels();
            
            // Start at the coarsest level + 1
            W ws = w.scaled(-numLevels());

            for (int lev = numLevels() - 1; lev >= 0; --lev) {
                setLevel(lev);
                ws = ws.scaled(1); // Scale up

                for (int iter = 0; iter < iterationsPerLevel; ++iter) {
                    
                    SingleStepResult<W> s = static_cast<D*>(this)->alignImpl(ws);
                    
                    const ScalarType newError = s.sumErrors / ScalarType(s.numConstraints);
                    const ScalarType errorChange = lastError() - newError;
                   
                    if (s.numConstraints > 0 &&
                        errorChange >= ScalarType(0) &&
                        (iter == 0 || (ScalarType)cv::norm(s.delta) >= eps))
                    {
                        static_cast<D*>(this)->applyStep(ws, s);
                        _error = newError;
                        
                        if (steps) steps->push_back(ws.scaled(lev));
                        
                    } else {
                        // Next level
                        break;
                    }
                }
                

            }
            w = ws;
            
            
            return *this;
        }
    
        
        /** 
            Return the total number of levels.
         */
        int numLevels() const {
            return _levels;
        }
        
        /**
            Access the error value from last iteration.
         
            \return the error value corresponding to last invocation of align.
        */
        ScalarType lastError() const {
            return _error;
        }
        
    protected:
        
        typedef typename W::Traits::PointType PointType;
    
        int level() const {
            return _level;
        }

        SelfType &setLevel(int level) {
            
            level = std::max<int>(0, std::min<int>(level, numLevels() - 1));
            _level = level;
            
            // Errors between levels are not compatible.
            _error = std::numeric_limits<ScalarType>::max();
            
            return *this;
        }
        
        cv::Mat templateImage() {
            return _templatePyramid[_level];
        }
        
        cv::Mat targetImage() {
            return _targetPyramid[_level];
        }
        
        ImagePyramid &templateImagePyramid() {
            return _templatePyramid;
        }
        
        ImagePyramid &targetImagePyramid() {
            return _targetPyramid;
        }
        
        /**
            Test if coordinates are in image.
            
            \param p Image coordinates
            \param imgSize Size of image
            \param r Minimum distance from image border pixels.
        */
        inline bool isInImage(const PointType &p, cv::Size imgSize, int r) const {
            int x = (int)std::floor(p(0)-ScalarType(0.5));
            int y = (int)std::floor(p(1)-ScalarType(0.5));
            
            return x >= r &&
                   y >= r &&
                   x < imgSize.width - r &&
                   y < imgSize.height - r;
        }

        
    private:
        
        ImagePyramid _templatePyramid;
        ImagePyramid _targetPyramid;
        
        int _levels;
        int _level;
        ScalarType _error;
    };
    
    
}

#endif