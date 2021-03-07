/*
* Vulkan Example - Hardware accelerated ray tracing callable shaders example
*
* Dynamically calls different shaders based on the geoemtry id in the closest hit shader
*
* Relevant code parts are marked with [POI]
*
* Copyright (C) 2021 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include "VulkanRaytracingSample.h"

class VulkanExample : public VulkanRaytracingSample
{
public:
	AccelerationStructure bottomLevelAS{};
	AccelerationStructure topLevelAS{};

	std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups{};
	struct ShaderBindingTables {
		ShaderBindingTable raygen;
		ShaderBindingTable miss;
		ShaderBindingTable hit;
		ShaderBindingTable callable;
	} shaderBindingTables;

	struct UniformData {
		glm::mat4 viewInverse;
		glm::mat4 projInverse;
	} uniformData;
	struct FrameObjects : public VulkanFrameObjects {
		vks::Buffer ubo;
		StorageImage storageImage;
		VkDescriptorSet descriptorSet;
	};
	std::vector<FrameObjects> frameObjects;

	VkPipeline pipeline;
	VkPipelineLayout pipelineLayout;
	VkDescriptorSetLayout descriptorSetLayout;

	vks::Buffer vertexBuffer;
	vks::Buffer indexBuffer;
	vks::Buffer transformBuffer;

	uint32_t objectCount = 3;

	// This sample is derived from an extended base class that saves most of the ray tracing setup boiler plate
	VulkanExample() : VulkanRaytracingSample()
	{
		title = "Ray tracing callable shaders";
		settings.overlay = false;
		timerSpeed *= 0.25f;
		camera.type = Camera::CameraType::lookat;
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 512.0f);
		camera.setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
		camera.setTranslation(glm::vec3(0.0f, 0.0f, -10.0f));
		enableExtensions();
	}

	~VulkanExample()
	{
		if (device) {
			vkDestroyPipeline(device, pipeline, nullptr);
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
			deleteAccelerationStructure(bottomLevelAS);
			deleteAccelerationStructure(topLevelAS);
			shaderBindingTables.raygen.destroy();
			shaderBindingTables.miss.destroy();
			shaderBindingTables.hit.destroy();
			shaderBindingTables.callable.destroy();
			vertexBuffer.destroy();
			indexBuffer.destroy();
			transformBuffer.destroy();
			for (FrameObjects& frame : frameObjects) {
				deleteStorageImage(frame.storageImage);
				frame.ubo.destroy();
				destroyFrameObjects(frame);
			}
		}
	}

	/*
		Create the bottom level acceleration structure contains the scene's actual geometry (vertices, triangles)
	*/
	void createBottomLevelAccelerationStructure()
	{
		// Setup vertices for a single triangle
		struct Vertex {
			float pos[3];
		};
		std::vector<Vertex> vertices = {
			{ {  1.0f,  1.0f, 0.0f } },
			{ { -1.0f,  1.0f, 0.0f } },
			{ {  0.0f, -1.0f, 0.0f } }
		};

		// Setup indices
		std::vector<uint32_t> indices = { 0, 1, 2 };
		uint32_t indexCount = static_cast<uint32_t>(indices.size());

		// Setup transform matrices for the geometries in the bottom level AS
		std::vector<VkTransformMatrixKHR> transformMatrices(objectCount);
		for (uint32_t i = 0; i < objectCount; i++) {
			transformMatrices[i] = {
				1.0f, 0.0f, 0.0f, (float)i * 3.0f - 3.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f
			};
		}
		// Transform buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&transformBuffer,
			objectCount * sizeof(VkTransformMatrixKHR),
			transformMatrices.data()));

		// Create buffers
		// For the sake of simplicity we won't stage the vertex data to the GPU memory
		// Vertex buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&vertexBuffer,
			vertices.size() * sizeof(Vertex),
			vertices.data()));
		// Index buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&indexBuffer,
			indices.size() * sizeof(uint32_t),
			indices.data()));

		VkDeviceOrHostAddressConstKHR vertexBufferDeviceAddress{};
		VkDeviceOrHostAddressConstKHR indexBufferDeviceAddress{};
		VkDeviceOrHostAddressConstKHR transformBufferDeviceAddress{};

		vertexBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(vertexBuffer.buffer);
		indexBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(indexBuffer.buffer);
		transformBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(transformBuffer.buffer);

		uint32_t numTriangles = 1;

		// Our scene will consist of three different triangles, that'll be distinguished in the shader via gl_GeometryIndexEXT, so we add three geometries to the bottom level AS
		std::vector<uint32_t> geometryCounts;
		std::vector<VkAccelerationStructureGeometryKHR> accelerationStructureGeometries;
		for (uint32_t i = 0; i < objectCount; i++) {
			VkAccelerationStructureGeometryKHR accelerationStructureGeometry = vks::initializers::accelerationStructureGeometryKHR();
			accelerationStructureGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
			accelerationStructureGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			accelerationStructureGeometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
			accelerationStructureGeometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
			accelerationStructureGeometry.geometry.triangles.vertexData = vertexBufferDeviceAddress;
			accelerationStructureGeometry.geometry.triangles.maxVertex = 3;
			accelerationStructureGeometry.geometry.triangles.vertexStride = sizeof(Vertex);
			accelerationStructureGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
			accelerationStructureGeometry.geometry.triangles.indexData = indexBufferDeviceAddress;
			accelerationStructureGeometry.geometry.triangles.transformData = transformBufferDeviceAddress;
			accelerationStructureGeometries.push_back(accelerationStructureGeometry);
			geometryCounts.push_back(1);
		}

		// Get size info
		VkAccelerationStructureBuildGeometryInfoKHR accelerationStructureBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
		accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		accelerationStructureBuildGeometryInfo.geometryCount = static_cast<uint32_t>(accelerationStructureGeometries.size());
		accelerationStructureBuildGeometryInfo.pGeometries = accelerationStructureGeometries.data();

		VkAccelerationStructureBuildSizesInfoKHR accelerationStructureBuildSizesInfo = vks::initializers::accelerationStructureBuildSizesInfoKHR();
		vkGetAccelerationStructureBuildSizesKHR(
			device,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			&accelerationStructureBuildGeometryInfo,
			geometryCounts.data(),
			&accelerationStructureBuildSizesInfo);

		createAccelerationStructure(bottomLevelAS, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, accelerationStructureBuildSizesInfo);

		// Create a small scratch buffer used during build of the bottom level acceleration structure
		ScratchBuffer scratchBuffer = createScratchBuffer(accelerationStructureBuildSizesInfo.buildScratchSize);

		VkAccelerationStructureBuildGeometryInfoKHR accelerationBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
		accelerationBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		accelerationBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		accelerationBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		accelerationBuildGeometryInfo.dstAccelerationStructure = bottomLevelAS.handle;
		accelerationBuildGeometryInfo.geometryCount = static_cast<uint32_t>(accelerationStructureGeometries.size());
		accelerationBuildGeometryInfo.pGeometries = accelerationStructureGeometries.data();
		accelerationBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

		// [POI] The bottom level acceleration structure for this sample contains three separate triangle geometries, so we can use gl_GeometryIndexEXT in the closest hit shader to select different callable shaders
		std::vector<VkAccelerationStructureBuildRangeInfoKHR> accelerationStructureBuildRangeInfos{};
		for (uint32_t i = 0; i < objectCount; i++) {
			VkAccelerationStructureBuildRangeInfoKHR accelerationStructureBuildRangeInfo{};
			accelerationStructureBuildRangeInfo.primitiveCount = numTriangles;
			accelerationStructureBuildRangeInfo.primitiveOffset = 0;
			accelerationStructureBuildRangeInfo.firstVertex = 0;
			accelerationStructureBuildRangeInfo.transformOffset = i * sizeof(VkTransformMatrixKHR);
			accelerationStructureBuildRangeInfos.push_back(accelerationStructureBuildRangeInfo);
		}
		std::vector<VkAccelerationStructureBuildRangeInfoKHR*> accelerationBuildStructureRangeInfos = { &accelerationStructureBuildRangeInfos[0], &accelerationStructureBuildRangeInfos[1], &accelerationStructureBuildRangeInfos[2] };

		if (accelerationStructureFeatures.accelerationStructureHostCommands)
		{
			// Implementation supports building acceleration structure building on host
			vkBuildAccelerationStructuresKHR(
				device,
				VK_NULL_HANDLE,
				1,
				&accelerationBuildGeometryInfo,
				accelerationBuildStructureRangeInfos.data());
		}
		else
		{
			// Acceleration structure needs to be build on the device
			VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
			vkCmdBuildAccelerationStructuresKHR(
				commandBuffer,
				1,
				&accelerationBuildGeometryInfo,
				accelerationBuildStructureRangeInfos.data());
			vulkanDevice->flushCommandBuffer(commandBuffer, queue);
		}

		deleteScratchBuffer(scratchBuffer);
	}

	/*
		The top level acceleration structure contains the scene's object instances
	*/
	void createTopLevelAccelerationStructure()
	{
		VkTransformMatrixKHR transformMatrix = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f };

		VkAccelerationStructureInstanceKHR instance{};
		instance.transform = transformMatrix;
		instance.instanceCustomIndex = 0;
		instance.mask = 0xFF;
		instance.instanceShaderBindingTableRecordOffset = 0;
		instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instance.accelerationStructureReference = bottomLevelAS.deviceAddress;

		// Buffer for instance data
		vks::Buffer instancesBuffer;
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&instancesBuffer,
			sizeof(VkAccelerationStructureInstanceKHR),
			&instance));

		VkDeviceOrHostAddressConstKHR instanceDataDeviceAddress{};
		instanceDataDeviceAddress.deviceAddress = getBufferDeviceAddress(instancesBuffer.buffer);

		VkAccelerationStructureGeometryKHR accelerationStructureGeometry = vks::initializers::accelerationStructureGeometryKHR();
		accelerationStructureGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
		accelerationStructureGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		accelerationStructureGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
		accelerationStructureGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
		accelerationStructureGeometry.geometry.instances.data = instanceDataDeviceAddress;

		// Get size info
		VkAccelerationStructureBuildGeometryInfoKHR accelerationStructureBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
		accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		accelerationStructureBuildGeometryInfo.geometryCount = 1;
		accelerationStructureBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;

		uint32_t primitive_count = 1;

		VkAccelerationStructureBuildSizesInfoKHR accelerationStructureBuildSizesInfo = vks::initializers::accelerationStructureBuildSizesInfoKHR();
		vkGetAccelerationStructureBuildSizesKHR(
			device,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			&accelerationStructureBuildGeometryInfo,
			&primitive_count,
			&accelerationStructureBuildSizesInfo);

		createAccelerationStructure(topLevelAS, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, accelerationStructureBuildSizesInfo);

		// Create a small scratch buffer used during build of the top level acceleration structure
		ScratchBuffer scratchBuffer = createScratchBuffer(accelerationStructureBuildSizesInfo.buildScratchSize);

		VkAccelerationStructureBuildGeometryInfoKHR accelerationBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
		accelerationBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		accelerationBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		accelerationBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		accelerationBuildGeometryInfo.dstAccelerationStructure = topLevelAS.handle;
		accelerationBuildGeometryInfo.geometryCount = 1;
		accelerationBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;
		accelerationBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

		VkAccelerationStructureBuildRangeInfoKHR accelerationStructureBuildRangeInfo{};
		accelerationStructureBuildRangeInfo.primitiveCount = 1;
		accelerationStructureBuildRangeInfo.primitiveOffset = 0;
		accelerationStructureBuildRangeInfo.firstVertex = 0;
		accelerationStructureBuildRangeInfo.transformOffset = 0;
		std::vector<VkAccelerationStructureBuildRangeInfoKHR*> accelerationBuildStructureRangeInfos = { &accelerationStructureBuildRangeInfo };

		if (accelerationStructureFeatures.accelerationStructureHostCommands)
		{
			// Implementation supports building acceleration structure building on host
			vkBuildAccelerationStructuresKHR(
				device,
				VK_NULL_HANDLE,
				1,
				&accelerationBuildGeometryInfo,
				accelerationBuildStructureRangeInfos.data());
		}
		else
		{
			// Acceleration structure needs to be build on the device
			VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
			vkCmdBuildAccelerationStructuresKHR(
				commandBuffer,
				1,
				&accelerationBuildGeometryInfo,
				accelerationBuildStructureRangeInfos.data());
			vulkanDevice->flushCommandBuffer(commandBuffer, queue);
		}

		deleteScratchBuffer(scratchBuffer);
		instancesBuffer.destroy();
	}

	/*
		Create the Shader Binding Tables that binds the programs and top-level acceleration structure
			
		SBT Layout used in this sample:

			/-----------\
			| raygen    |
			|-----------|
			| miss      |
			|-----------|
			| hit       |
			|-----------|
			| callable0 |
			| callable1 |
			| callabel2 |
			\-----------/

	*/
	void createShaderBindingTables() {
		const uint32_t handleSize = rayTracingPipelineProperties.shaderGroupHandleSize;
		const uint32_t handleSizeAligned = vks::tools::alignedSize(rayTracingPipelineProperties.shaderGroupHandleSize, rayTracingPipelineProperties.shaderGroupHandleAlignment);
		const uint32_t groupCount = static_cast<uint32_t>(shaderGroups.size());
		const uint32_t sbtSize = groupCount * handleSizeAligned;

		std::vector<uint8_t> shaderHandleStorage(sbtSize);
		VK_CHECK_RESULT(vkGetRayTracingShaderGroupHandlesKHR(device, pipeline, 0, groupCount, sbtSize, shaderHandleStorage.data()));

		createShaderBindingTable(shaderBindingTables.raygen, 1);
		createShaderBindingTable(shaderBindingTables.miss, 1);
		createShaderBindingTable(shaderBindingTables.hit, 1);
		// [POI] The callable shader binding table contains one shader handle per ray traced object
		createShaderBindingTable(shaderBindingTables.callable, objectCount);

		// Copy handles
		memcpy(shaderBindingTables.raygen.mapped, shaderHandleStorage.data(), handleSize);
		memcpy(shaderBindingTables.miss.mapped, shaderHandleStorage.data() + handleSizeAligned, handleSize);
		memcpy(shaderBindingTables.hit.mapped, shaderHandleStorage.data() + handleSizeAligned * 2, handleSize);
		memcpy(shaderBindingTables.callable.mapped, shaderHandleStorage.data() + handleSizeAligned * 3, handleSize * 3);
	}

	/*
		Create the descriptor sets used for the ray tracing dispatch
	*/
	void createDescriptorSets()
	{
		const uint32_t frameCount = static_cast<uint32_t>(frameObjects.size());
		std::vector<VkDescriptorPoolSize> poolSizes = {
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * frameCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 * frameCount },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * frameCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * frameCount }
		};
		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, frameCount);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, nullptr, &descriptorPool));

		for (FrameObjects& frame : frameObjects) {
			VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayout, 1);
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &frame.descriptorSet));

			VkWriteDescriptorSetAccelerationStructureKHR descriptorAccelerationStructureInfo = vks::initializers::writeDescriptorSetAccelerationStructureKHR();
			descriptorAccelerationStructureInfo.accelerationStructureCount = 1;
			descriptorAccelerationStructureInfo.pAccelerationStructures = &topLevelAS.handle;

			VkWriteDescriptorSet accelerationStructureWrite{};
			accelerationStructureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			// The specialized acceleration structure descriptor has to be chained
			accelerationStructureWrite.pNext = &descriptorAccelerationStructureInfo;
			accelerationStructureWrite.dstSet = frame.descriptorSet;
			accelerationStructureWrite.dstBinding = 0;
			accelerationStructureWrite.descriptorCount = 1;
			accelerationStructureWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

			VkDescriptorImageInfo storageImageDescriptor{ VK_NULL_HANDLE, frame.storageImage.view, VK_IMAGE_LAYOUT_GENERAL };
			VkDescriptorBufferInfo vertexBufferDescriptor{ vertexBuffer.buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo indexBufferDescriptor{ indexBuffer.buffer, 0, VK_WHOLE_SIZE };

			std::vector<VkWriteDescriptorSet> writeDescriptorSets = {
				// Binding 0: Top level acceleration structure
				accelerationStructureWrite,
				// Binding 1: Ray tracing result image
				vks::initializers::writeDescriptorSet(frame.descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &storageImageDescriptor),
				// Binding 2: Uniform data
				vks::initializers::writeDescriptorSet(frame.descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, &frame.ubo.descriptor),
				// Binding 3: Scene vertex buffer
				vks::initializers::writeDescriptorSet(frame.descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, &vertexBufferDescriptor),
				// Binding 4: Scene index buffer
				vks::initializers::writeDescriptorSet(frame.descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, &indexBufferDescriptor),
			};
			vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, VK_NULL_HANDLE);
		}
	}

	/*
		Create our ray tracing pipeline
	*/
	void createRayTracingPipeline()
	{
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			// Binding 0: Acceleration structure
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 0),
			// Binding 1: Storage image
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 1),
			// Binding 2: Uniform buffer
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR, 2),
			// Binding 3: Vertex buffer 
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 3),
			// Binding 4: Index buffer
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 4),
		};

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayout));

		VkPipelineLayoutCreateInfo pPipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayout, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCI, nullptr, &pipelineLayout));
	
		/*
			Setup ray tracing shader groups
		*/
		std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
		VkRayTracingShaderGroupCreateInfoKHR shaderGroup;

		// Ray generation shader group
		shaderStages.push_back(loadShader(getShadersPath() + "raytracingcallable/raygen.rgen.spv", VK_SHADER_STAGE_RAYGEN_BIT_KHR));
		shaderGroup = vks::initializers::rayTracingShaderGroupCreateInfoKHR();
		shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
		shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
		shaderGroups.push_back(shaderGroup);

		// Miss shader group
		shaderStages.push_back(loadShader(getShadersPath() + "raytracingcallable/miss.rmiss.spv", VK_SHADER_STAGE_MISS_BIT_KHR));
		shaderGroup = vks::initializers::rayTracingShaderGroupCreateInfoKHR();
		shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
		shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
		shaderGroups.push_back(shaderGroup);

		// Closest hit shader group
		shaderStages.push_back(loadShader(getShadersPath() + "raytracingcallable/closesthit.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
		shaderGroup = vks::initializers::rayTracingShaderGroupCreateInfoKHR();
		shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
		shaderGroup.generalShader = VK_SHADER_UNUSED_KHR;
		shaderGroup.closestHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
		shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
		shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
		shaderGroups.push_back(shaderGroup);

		// [POI] Callable shader group
		// This sample's hit shader will call different callable shaders depending on the geometry index using executeCallableEXT, so as we render three geometries, we'll also use three callable shaders
		for (uint32_t i = 0; i < objectCount; i++) 
		{
			shaderStages.push_back(loadShader(getShadersPath() + "raytracingcallable/callable" + std::to_string(i+1) + ".rcall.spv", VK_SHADER_STAGE_CALLABLE_BIT_KHR));
			shaderGroup = vks::initializers::rayTracingShaderGroupCreateInfoKHR();
			shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
			shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
			shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
			shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
			shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
			shaderGroups.push_back(shaderGroup);
		}

		VkRayTracingPipelineCreateInfoKHR rayTracingPipelineCI = vks::initializers::rayTracingPipelineCreateInfoKHR();
		rayTracingPipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		rayTracingPipelineCI.pStages = shaderStages.data();
		rayTracingPipelineCI.groupCount = static_cast<uint32_t>(shaderGroups.size());
		rayTracingPipelineCI.pGroups = shaderGroups.data();
		rayTracingPipelineCI.maxPipelineRayRecursionDepth = 2;
		rayTracingPipelineCI.layout = pipelineLayout;
		VK_CHECK_RESULT(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rayTracingPipelineCI, nullptr, &pipeline));
	}

	void getEnabledFeatures()
	{
		// Enable features required for ray tracing using feature chaining via pNext		
		enabledBufferDeviceAddresFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		enabledBufferDeviceAddresFeatures.bufferDeviceAddress = VK_TRUE;

		enabledRayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
		enabledRayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;
		enabledRayTracingPipelineFeatures.pNext = &enabledBufferDeviceAddresFeatures;

		enabledAccelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		enabledAccelerationStructureFeatures.accelerationStructure = VK_TRUE;
		enabledAccelerationStructureFeatures.pNext = &enabledRayTracingPipelineFeatures;

		deviceCreatepNextChain = &enabledAccelerationStructureFeatures;
	}

	void prepare()
	{
		VulkanRaytracingSample::prepare();

		// Prepare per-frame ressources
		frameObjects.resize(swapChain.imageCount);
		for (FrameObjects& frame : frameObjects) {
			// Base objects
			createFrameObjects(frame);
			// Storage images for ray tracing output
			createStorageImage(frame.storageImage, swapChain.colorFormat, { width, height, 1 });
			// Uniform buffers
			VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &frame.ubo, sizeof(uniformData), &uniformData));
			VK_CHECK_RESULT(frame.ubo.map());
		}

		// Create the acceleration structures used to render the ray traced scene
		createBottomLevelAccelerationStructure();
		createTopLevelAccelerationStructure();

		createRayTracingPipeline();
		createShaderBindingTables();
		createDescriptorSets();
		buildCommandBuffers();
		prepared = true;
	}

	void draw()
	{
		// If the window has been resized, we need to recreate the storage image and it's descriptor
		if (resized)
		{
			vkDeviceWaitIdle(device);
			for (FrameObjects& frame : frameObjects) {
				// Recreate storage image
				createStorageImage(frame.storageImage, swapChain.colorFormat, { width, height, 1 });
				// Update descriptor using the storage image
				VkDescriptorImageInfo storageImageDescriptor{ VK_NULL_HANDLE, frame.storageImage.view, VK_IMAGE_LAYOUT_GENERAL };
				VkWriteDescriptorSet resultImageWrite = vks::initializers::writeDescriptorSet(frame.descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &storageImageDescriptor);
				vkUpdateDescriptorSets(device, 1, &resultImageWrite, 0, VK_NULL_HANDLE);
			}
		}

		FrameObjects currentFrame = frameObjects[currentBuffer];
		
		VulkanExampleBase::prepareFrame(currentFrame);

		VK_CHECK_RESULT(vkWaitForFences(device, 1, &currentFrame.renderCompleteFence, VK_TRUE, UINT64_MAX));
		VK_CHECK_RESULT(vkResetFences(device, 1, &currentFrame.renderCompleteFence));

		if (!paused || camera.updated) {
			uniformData.projInverse = glm::inverse(camera.matrices.perspective);
			uniformData.viewInverse = glm::inverse(camera.matrices.view);
			memcpy(currentFrame.ubo.mapped, &uniformData, sizeof(uniformData));
		}

		// Build the command buffer
		const VkCommandBuffer commandBuffer = currentFrame.commandBuffer;
		const VkImage swapChainImage = swapChain.images[currentBuffer];
		const VkCommandBufferBeginInfo commandBufferBeginInfo = vks::initializers::commandBufferBeginInfo();

		VK_CHECK_RESULT(vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo));

		/*
			Dispatch the ray tracing commands
		*/
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout, 0, 1, &currentFrame.descriptorSet, 0, 0);

		vkCmdTraceRaysKHR(
			commandBuffer,
			&shaderBindingTables.raygen.stridedDeviceAddressRegion,
			&shaderBindingTables.miss.stridedDeviceAddressRegion,
			&shaderBindingTables.hit.stridedDeviceAddressRegion,
			// [POI] Bind the callable shader table
			&shaderBindingTables.callable.stridedDeviceAddressRegion,
			width,
			height,
			1);

		/*
			Copy ray tracing output to swap chain image
		*/

		const VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		// Prepare current swap chain image as transfer destination
		vks::tools::setImageLayout(
			commandBuffer,
			swapChainImage,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			subresourceRange);

		// Prepare ray tracing output image as transfer source
		vks::tools::setImageLayout(
			commandBuffer,
			currentFrame.storageImage.image,
			VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			subresourceRange);

		VkImageCopy copyRegion{};
		copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyRegion.srcOffset = { 0, 0, 0 };
		copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyRegion.dstOffset = { 0, 0, 0 };
		copyRegion.extent = { width, height, 1 };
		vkCmdCopyImage(commandBuffer, currentFrame.storageImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapChainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		// Transition swap chain image back for presentation
		vks::tools::setImageLayout(
			commandBuffer,
			swapChainImage,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			subresourceRange);

		// Transition ray tracing output image back to general layout
		vks::tools::setImageLayout(
			commandBuffer,
			currentFrame.storageImage.image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_GENERAL,
			subresourceRange);

		VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer));
	
		VulkanExampleBase::submitFrame(currentFrame);
	}

	virtual void render()
	{
		if (prepared) {
			draw();
		}
	}
};

VULKAN_EXAMPLE_MAIN()