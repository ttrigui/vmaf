/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "funque_vif_options.h"

#if USE_DYNAMIC_SIGMA_NSQ
int compute_vif_funque(const float* x, const float* y, size_t width, size_t height, 
                        double* score, double* score_num, double* score_den, int k, 
                        int stride, double sigma_nsq_arg, int vif_level);
#else
int compute_vif_funque(const float* x, const float* y, size_t width, size_t height, 
                        double* score, double* score_num, double* score_den, int k, 
                        int stride, double sigma_nsq_arg);
#endif