/**
 * @file job.h
 * @author Toesoe (code at toesoe dot dev)
 * @brief produce DescriptorFields based on a validated request
 * @version 0.1
 * @date 17-11-2026
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef JOB_H_
#define JOB_H_

#include "request.h"
#include "src/core/wire/descriptor.h"

/**
 * @brief build a descrriptor from a set of validated parameters
 *
 * @param pParams pointer to input parameters
 * @param pDescriptorOut output for descriptor
 * @return int status code
 */
int scanBuildDescriptor(const struct DescriptorParameters *pParams, struct Descriptor *pDescriptorOut);

#endif //! JOB_H_