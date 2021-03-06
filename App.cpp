#include <string>
#include <fstream>
#include <streambuf>
#include <string>
#include <math.h>

#include "GlUtil.h"
#include "Geom.h"
#include "App.h"

App::App( int argc, char *argv[] ) {
	monitorWindow = NULL;
	monitorGlContext = NULL;
	monitorWinWidth = 640;
	monitorWinHeight = 320;
	sceneShader = 0;
	monitorWindowShader = 0;
	controllerShader = 0;
	renderModelShader = 0;
	hmd = NULL;
	controllerVertBuffer = 0;
	controllerVertAr = 0;
	sceneVertexAr = 0;
	sceneShaderMatrix = -1;
	controllerShaderMatrix = -1;
	renderModelShaderMatrix = -1;
	poseClasses = "";
	buttonPressed = false;
	mode = none;

	// other initialization tasks are done in BInit
	memset(classForDeviceIdx, 0, sizeof(classForDeviceIdx));
};

App::~App() {
	// work is done in Shutdown
	printf( "Shutdown" );
}

// --------------------------------- main ------------------------------------
void App::mainLoop() {
	bool bQuit = false;

	SDL_StartTextInput();
	SDL_ShowCursor(SDL_DISABLE);

	while (!bQuit) {
		bQuit = handleInput();
		renderFrame();
	}

	SDL_StopTextInput();
}

Vector2 App::getSnap(Vector3 target3d) {
	float bestDist = std::numeric_limits<float>::infinity();
	Vector2 target2d = Vector2(target3d.x, target3d.z);
	Vector2 bestSnap = target2d;

	// Snap to compass rose when drawing first segment
	if (mode == draw && currentPolygon.getVertexCount() == 2) {
		for (float ang = -M_PI; ang < M_PI; ang += M_PI / 2) {
			Vector2 dir(cosf(ang), sinf(ang));
			Vector2 firstVert = currentPolygon.getFirstVertex();
			Vector2 point = target2d - firstVert;
			float dist = dir.dot(point);
			dist = roundf(dist * 100) / 100;
			Vector2 pointOnLine = firstVert + dir * dist;
			float error = (target2d - pointOnLine).length();
			if (error < bestDist) {
				bestDist = error;
				bestSnap = pointOnLine;
			}
		}
	}

	// Snap to compass rose when drawing first segment
	if (mode == draw && currentPolygon.getVertexCount() >= 3) {
		Vector2 fdir = currentPolygon.getSecondToLast() - currentPolygon.getThirdToLast();
		fdir.normalize();
		float sdir = atan2(fdir.y, fdir.x);
		for (float ang = sdir - M_PI; ang < sdir + M_PI; ang += M_PI / 2) {
			Vector2 dir(cosf(ang), sinf(ang));
			Vector2 firstVert = currentPolygon.getSecondToLast();
			Vector2 point = target2d - firstVert;
			float dist = dir.dot(point);
			dist = roundf(dist * 100) / 100;
			Vector2 pointOnLine = firstVert + dir * dist;
			float error = (target2d - pointOnLine).length();
			if (error < bestDist) {
				bestDist = error;
				bestSnap = pointOnLine;
			}
		}
	}

	// Snap to first point
	if (mode == draw && currentPolygon.getVertexCount() >= 3) {
		float dist = (target2d - currentPolygon.getFirstVertex()).length();
		if (dist < 0.01f) { 
			bestDist = dist;
			bestSnap = currentPolygon.getFirstVertex();
		}
	}

	return bestSnap;
}

bool App::handleInput() {

	// SDL input
	SDL_Event sdlEvent;
	bool quit = false;
	while (SDL_PollEvent(&sdlEvent) != 0) {
		if (sdlEvent.type == SDL_QUIT) {
			quit = true;
		} else if (sdlEvent.type == SDL_KEYDOWN) {
			if (sdlEvent.key.keysym.sym == SDLK_ESCAPE || sdlEvent.key.keysym.sym == SDLK_q) {
				quit = true;
			}
		}
	}

	// Process SteamVR events
	vr::VREvent_t event;
	while (hmd->PollNextEvent(&event, sizeof(event))) {
		if(event.eventType == vr::VREvent_TrackedDeviceActivated) {
			initDeviceModel(event.trackedDeviceIndex);
		}
	}

	// Process SteamVR controller state
	bool anyButtonPressed = false;
	for (vr::TrackedDeviceIndex_t deviceIdx = 0; deviceIdx < vr::k_unMaxTrackedDeviceCount; deviceIdx++) {
		vr::VRControllerState_t controllerState;
		if (!hmd->GetControllerState(deviceIdx, &controllerState, sizeof(controllerState))) {
			continue; // Unable to get state
		}
		const vr::TrackedDevicePose_t & pose = devicePose[deviceIdx];
		if (!pose.bPoseIsValid) {
			continue; // Invalid pose (not tracking?)
		}
		if (hmd->GetTrackedDeviceClass(deviceIdx) != vr::TrackedDeviceClass_Controller) {
			continue;
		}
		if (deviceIdx == 0) {
			continue; // headset?
		}
		Matrix4 torsoInverse = torsoPose;
		torsoInverse.invert();
		const Matrix4 controllerMat = torsoInverse * devicePoseMat[deviceIdx];
		Vector3 floorNorm(0, 1, 0); // Floor points up
		Vector3 worldOrigin(0, 0, 0); // World origin is on our plane
		Vector3 laserOrigin = controllerMat * Vector3(0, 0, 0);
		Vector3 laserDir = (controllerMat * Vector3(0, 0, 1)) - laserOrigin;
		Vector3 rightDir = laserDir.cross(floorNorm);
		Vector3 laserFloorIsec;
		float t = geom::rayPlaneIsec(floorNorm, worldOrigin, laserOrigin, laserDir, laserFloorIsec);
		Vector2 isec2d = getSnap(laserOrigin);

		// Drawing
		if (mode == draw /*&& t < 0*/) {
			currentPolygon.updateLastVertex(Vector2(isec2d.x, isec2d.y));
			Vector2 last = currentPolygon.getLastVertex();
			Vector2 second = currentPolygon.getSecondToLast();
			Vector2 dir = last - second;
			dir.normalize();
			float length = (last - second).length();

			text = measure(length);

			Matrix4 mat;
			mat.scale(0.1f);
			mat.rotateY(dir.x, -dir.y);
			mat.translate(second.x, currentPolygon.getBase(), second.y);
			textPos = mat;
		}
		if (mode == extrude && deviceIdx == currentController) {
			Vector2 first = currentPolygon.getFirstVertex();
			Vector2 second = currentPolygon.getSecondVertex();
			Vector3 wallNorm = Vector3(0, 1, 0).cross(Vector3(second.x - first.x, 0, second.y - first.y));
			Vector3 pointOnWall = Vector3(first.x, 0, first.y);
			Vector3 wallIsec;
			//float wallT = geom::rayPlaneIsec(wallNorm, pointOnWall, laserOrigin, laserDir, wallIsec);
			//if (wallT < 0) {
				currentPolygon.setHeight(laserOrigin.y - currentPolygon.getBase());
			//}
		}

		// Click handlers
		if (buttonPressed == false && controllerState.ulButtonPressed & BTN_TRIGGER) {
			triggerPressed(deviceIdx, t, isec2d, laserOrigin);
		}
		if (buttonPressed == false && controllerState.ulButtonPressed & BTN_GRIP) {
			gripWorld = worldTrans;
			gripInverse = gripWorld;
			gripInverse.invert();
			gripLeft = leftHandPose * Vector3(0, 0, 0);
			gripRight = rightHandPose * Vector3(0, 0, 0);
			gripLeft = gripInverse * gripLeft;
			gripRight = gripInverse * gripRight;
		}
		if (buttonPressed == true && controllerState.ulButtonPressed & BTN_GRIP) {
			Vector3 gripCenter = (gripRight - gripLeft) / 2.0f + gripLeft;
			Vector3 gripDir = gripRight - gripCenter;

			Vector3 leftHand = leftHandPose * Vector3(0, 0, 0);
			Vector3 rightHand = rightHandPose * Vector3(0, 0, 0);

			Vector3 worldLeft = gripInverse * leftHand;
			Vector3 worldRight = gripInverse * rightHand;
			Vector3 worldCenter = (worldRight - worldLeft) / 2.0f + worldLeft;
			Vector3 worldDir = worldRight - worldCenter;
			Vector3 delta = worldCenter - gripCenter;
			float deltaAng = atan2f(gripDir.z, gripDir.x) - atan2f(worldDir.z, worldDir.x);
			float deltaDist = worldDir.length() / gripDir.length();

			Matrix4 trans;

			trans.translate(-gripCenter);
			trans.rotateY(deltaAng * 180.0f / M_PI);
			trans.scale(deltaDist);
			trans.translate(gripCenter);

			trans.translate(delta);

			worldTrans = gripWorld * trans;
		}

		// Button tracking
		if (controllerState.ulButtonPressed != 0) {
			anyButtonPressed = true;
			buttonPressed = true;

			/*
			Matrix4 textOrigin;
			textOrigin.translate(-1.0f, 0, 0.0f);
			textOrigin.scale(0.25);
			textPos = textOrigin;
			text = byteToBinary(controllerState.ulButtonPressed);
			*/
		}

		// Fly
		Vector2 input(controllerState.rAxis[0].x, controllerState.rAxis[0].y);
		if (fabs(input.y) >= 0.2f) {
			float offset = input.y * 0.2f;
			Matrix4 mat = devicePoseMat[deviceIdx];
			Vector3 controllerOrigin = mat * Vector3(0, 0, 0);
			Vector3 controllerDir = (mat * Vector3(0, 0, 1)) - controllerOrigin;
			Vector3 moveDir = controllerDir * (input.y - offset) * 0.05;
			torsoPose.translate(moveDir);
		}
		if (fabs(input.x) >= 0.2f) {
			float offset = input.x * 0.2f;
			torsoPose.rotateY(input.x - offset);
		}
	}
	regenVB();
	if (anyButtonPressed == false) {
		buttonPressed = false;
	}

	return quit;
}

std::string App::measure(float length) {
	char buff[100];
	snprintf(buff, sizeof(buff), "%.1fm", length);
	if (length <= 1) {
		snprintf(buff, sizeof(buff), "%.0fcm", length * 100);
	}
	if (length <= 0.01) {
		snprintf(buff, sizeof(buff), "%.0fmm", length * 1000);
	} 
	return std::string(buff);
}

const char *App::byteToBinary(int x) {
	static char b[9];
	b[0] = '\0';
	for (int z = 128; z > 0; z >>= 1) {
		strcat(b, ((x & z) == z) ? "1" : "0");
	}
	return b;
}

void App::triggerPressed(vr::TrackedDeviceIndex_t deviceIdx, float t, Vector2 isec2d, Vector3 laserOrigin) {
	if (mode == none) {
		//if (t < 0) {
			currentController = deviceIdx;
			currentPolygon.clear();
			currentPolygon.setBase(laserOrigin.y);
			currentPolygon.setHeight(0);
			currentPolygon.addVertex(Vector2(laserOrigin.x, laserOrigin.z));
			currentPolygon.addVertex(Vector2(laserOrigin.x, laserOrigin.z));
			mode = draw;
		//}
	} else {
		if (mode == draw) {
			//if (t < 0) {
				if (isec2d == currentPolygon.getFirstVertex()) {
					mode = extrude;
				}
				else {
					currentPolygon.addVertex(Vector2(laserOrigin.x, laserOrigin.z));
				}
			//}
		} else if (mode == extrude) {
			polygons.push_back(currentPolygon);
			currentPolygon.clear();
			mode = none;
		}
	}
}

void App::regenVB() {
	std::vector<float> floatAr;

	for (auto polygon = polygons.begin(); polygon < polygons.end(); ++polygon) {
		polygon->renderRoom(floatAr);
	}

	sceneVertCount = floatAr.size() / 5;
	if (sceneVertexAr != 0) {
		glDeleteVertexArrays(1, &sceneVertexAr);
		sceneVertexAr = 0;
	}
	if (sceneVertBuffer) {
		glDeleteBuffers(1, &sceneVertBuffer);
		sceneVertBuffer = 0;
	}
	if (sceneVertCount == 0) {
		return;
	}

	glGenVertexArrays(1, &sceneVertexAr);
	glBindVertexArray(sceneVertexAr);

	glGenBuffers(1, &sceneVertBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, sceneVertBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * floatAr.size(), &floatAr[0], GL_STATIC_DRAW);

	GLsizei stride = sizeof(VertexDataScene);
	uintptr_t offset = 0;

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)offset);

	offset += sizeof(Vector3);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)offset);

	glBindVertexArray(0);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
}

void App::renderText(GLuint s, std::string text, GLfloat x, GLfloat y, GLfloat scale, glm::vec3 color) {

	// Iterate through all characters
	std::string::const_iterator c;
	for (c = text.begin(); c != text.end(); c++) {
		Character ch = Characters[*c];

		GLfloat xpos = x + ch.Bearing.x * scale;
		GLfloat ypos = y - (ch.Size.y - ch.Bearing.y) * scale;

		GLfloat w = ch.Size.x * scale;
		GLfloat h = ch.Size.y * scale;
		// Update VBO for each character
		GLfloat vertices[6][4] = {
			{ xpos,     ypos + h,   0.0, 0.0 },
			{ xpos,     ypos,       0.0, 1.0 },
			{ xpos + w, ypos,       1.0, 1.0 },

			{ xpos,     ypos + h,   0.0, 0.0 },
			{ xpos + w, ypos,       1.0, 1.0 },
			{ xpos + w, ypos + h,   1.0, 0.0 }
		};
		// Render glyph texture over quad
		glBindTexture(GL_TEXTURE_2D, ch.textureID);
		// Update content of VBO memory
		glBindBuffer(GL_ARRAY_BUFFER, VBO);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		// Render quad
		glDrawArrays(GL_TRIANGLES, 0, 6);
		// Now advance cursors for next glyph (note that advance is number of 1/64 pixels)
		x += (ch.Advance >> 6) * scale; // Bitshift by 6 to get value in pixels (2^6 = 64)
	}
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void App::renderFrame() {
	if (hmd) {
		renderControllerAxes();

		renderToHmd();
		renderToMonitorWindow();

		vr::Texture_t leftEyeTexture = { (void*)leftEyeDesc.resolveTextureId, vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
		vr::VRCompositor()->Submit(vr::Eye_Left, &leftEyeTexture);
		vr::Texture_t rightEyeTexture = { (void*)rightEyeDesc.resolveTextureId, vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
		vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTexture);
	}

	// Swap
	SDL_GL_SwapWindow(monitorWindow);

	// We want to make sure the glFinish waits for the entire present to complete, not just the submission
	// of the command. So, we do a clear here right here so the glFinish will wait fully for the swap.
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	updateHmdPose();
}

void App::renderControllerAxes() {
	// don't draw controllers if somebody else has input focus
	if (hmd->IsInputFocusCapturedByAnotherProcess())
		return;

	std::vector<float> floatAr;

	controllerVertCount = 0;

	for (vr::TrackedDeviceIndex_t deviceIdx = vr::k_unTrackedDeviceIndex_Hmd + 1; deviceIdx < vr::k_unMaxTrackedDeviceCount; ++deviceIdx) {
		if (!hmd->IsTrackedDeviceConnected(deviceIdx))
			continue;

		if (hmd->GetTrackedDeviceClass(deviceIdx) != vr::TrackedDeviceClass_Controller)
			continue;

		if (!devicePose[deviceIdx].bPoseIsValid)
			continue;

		Matrix4 torsoInverse = torsoPose;
		torsoInverse.invert();
		const Matrix4 & mat = torsoInverse * devicePoseMat[deviceIdx];

		Vector4 center = mat * Vector4(0, 0, 0, 1);

		Vector4 start = mat * Vector4(0, 0, 0.0f, 1);
		Vector4 end = mat * Vector4(0, 0, -39.f, 1);
		Vector3 color(0, 0, 1);
		if (deviceIdx == 2) {
			color = Vector3(1, 0, 0);
		}

		floatAr.push_back(start.x); floatAr.push_back(start.y); floatAr.push_back(start.z);
		floatAr.push_back(color.x); floatAr.push_back(color.y); floatAr.push_back(color.z);

		floatAr.push_back(end.x); floatAr.push_back(end.y); floatAr.push_back(end.z);
		floatAr.push_back(color.x); floatAr.push_back(color.y); floatAr.push_back(color.z);
	}

	// ---------------------------------------------- TODO: Move polygon rendering out of axes rendering & apply worldTrans to polygons ---------------------------------------------
	if (mode == draw) {
		currentPolygon.renderLines(floatAr);
	} else if (mode == extrude) {
		currentPolygon.renderLines(floatAr);
		regenVB();
	}
	controllerVertCount = floatAr.size() / 6;

	// Setup the VAO the first time through.
	if (controllerVertAr == 0) {
		glGenVertexArrays(1, &controllerVertAr);
		glBindVertexArray(controllerVertAr);

		glGenBuffers(1, &controllerVertBuffer);
		glBindBuffer(GL_ARRAY_BUFFER, controllerVertBuffer);

		GLuint stride = 2 * 3 * sizeof(float);
		GLuint offset = 0;

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)offset);

		offset += sizeof(Vector3);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)offset);

		glBindVertexArray(0);
	}

	glBindBuffer(GL_ARRAY_BUFFER, controllerVertBuffer);

	// set vertex data if we have some
	if (floatAr.size() > 0) {
		//$ TODO: Use glBufferSubData for this...
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * floatAr.size(), &floatAr[0], GL_STREAM_DRAW);
	}
}

void App::renderToHmd() {
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_MULTISAMPLE);

	// Left Eye
	glBindFramebuffer(GL_FRAMEBUFFER, leftEyeDesc.renderFramebufferId);
	glViewport(0, 0, hmdRenderWidth, hmdRenderHeight);
	renderToEye(vr::Eye_Left);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glDisable(GL_MULTISAMPLE);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, leftEyeDesc.renderFramebufferId);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, leftEyeDesc.resolveFramebufferId);

	glBlitFramebuffer(0, 0, hmdRenderWidth, hmdRenderHeight, 0, 0, hmdRenderWidth, hmdRenderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	glEnable(GL_MULTISAMPLE);

	// Right Eye
	glBindFramebuffer(GL_FRAMEBUFFER, rightEyeDesc.renderFramebufferId);
	glViewport(0, 0, hmdRenderWidth, hmdRenderHeight);
	renderToEye(vr::Eye_Right);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glDisable(GL_MULTISAMPLE);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, rightEyeDesc.renderFramebufferId);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rightEyeDesc.resolveFramebufferId);

	glBlitFramebuffer(0, 0, hmdRenderWidth, hmdRenderHeight, 0, 0, hmdRenderWidth, hmdRenderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void App::renderToEye(vr::Hmd_Eye eye) {
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);

	Matrix4 trans = getEyeProjMat(eye) * worldTrans;
	glUseProgram(sceneShader);
	glUniformMatrix4fv(sceneShaderMatrix, 1, GL_FALSE, trans.get());
	glBindVertexArray(sceneVertexAr);
	glBindTexture(GL_TEXTURE_2D, brickTextureId);
	glDrawArrays(GL_TRIANGLES, 0, sceneVertCount);
	glBindVertexArray(0);

	// Text
	Matrix4 textMat = getEyeProjMat(eye) * worldTrans;
	textMat *= textPos;
	glUseProgram(fontShader);
	glUniform3f(textColor, 1.0f, 1.0f, 1.0f);
	glUniformMatrix4fv(sceneShaderMatrix, 1, GL_FALSE, textMat.get());
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(VAO);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	renderText(fontShader, text, 0, 0, 0.01f, glm::vec3(1.0f, 1.0f, 1.0f));

	// Axises
	bool inputCaptured = hmd->IsInputFocusCapturedByAnotherProcess();
	if (!inputCaptured) {
		glUseProgram(controllerShader);
		glUniformMatrix4fv(controllerShaderMatrix, 1, GL_FALSE, getEyeProjMat(eye).get());
		glBindVertexArray(controllerVertAr);
		glDrawArrays(GL_LINES, 0, controllerVertCount);
		glBindVertexArray(0);
	}

	// ----- Render Model rendering -----
	glUseProgram(renderModelShader);
	for (uint32_t deviceIdx = 0; deviceIdx < vr::k_unMaxTrackedDeviceCount; deviceIdx++) {
		if (!trackedDeviceModels[deviceIdx])
			continue;
		const vr::TrackedDevicePose_t & pose = devicePose[deviceIdx];
		if (!pose.bPoseIsValid)
			continue;
		if (inputCaptured && hmd->GetTrackedDeviceClass(deviceIdx) == vr::TrackedDeviceClass_Controller)
			continue;

		Matrix4 torsoInverse = torsoPose;
		torsoInverse.invert();
		const Matrix4 & matDeviceToTracking = devicePoseMat[deviceIdx];
		Matrix4 matMVP = getEyeProjMat(eye);
		if (hmd->GetTrackedDeviceClass(deviceIdx) == vr::TrackedDeviceClass_Controller) {
			matMVP *= torsoInverse;
		}
		matMVP *= matDeviceToTracking;
		glUniformMatrix4fv(renderModelShaderMatrix, 1, GL_FALSE, matMVP.get());
		//trackedDeviceModels[deviceIdx]->Draw();
	}

	glUseProgram(0);
}

Matrix4 App::getEyeProjMat(vr::Hmd_Eye eye) {
	if (eye == vr::Eye_Left) {
		return leftEyeProj * leftEyePos * inverseHmdPose * torsoPose;
	} else if (eye == vr::Eye_Right) {
		return rightEyeProj * rightEyePos * inverseHmdPose * torsoPose;
	}
	Matrix4 matMVP;
	return matMVP;
}

void App::renderToMonitorWindow()
{
	glDisable(GL_DEPTH_TEST);
	glViewport(0, 0, monitorWinWidth, monitorWinHeight);

	glBindVertexArray(monitorWinVertAr);
	glUseProgram(monitorWindowShader);

	// render left eye (first half of index array )
	glBindTexture(GL_TEXTURE_2D, leftEyeDesc.resolveTextureId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glDrawElements(GL_TRIANGLES, monitorWinIdxSize / 2, GL_UNSIGNED_SHORT, 0);

	// render right eye (second half of index array )
	glBindTexture(GL_TEXTURE_2D, rightEyeDesc.resolveTextureId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glDrawElements(GL_TRIANGLES, monitorWinIdxSize / 2, GL_UNSIGNED_SHORT, (const void *)(monitorWinIdxSize));

	glBindVertexArray(0);
	glUseProgram(0);
}

void App::updateHmdPose() {
	if (!hmd)
		return;

	vr::VRCompositor()->WaitGetPoses(devicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0);

	poseClasses = "";
	int hand = 0;
	for (int deviceIdx = 0; deviceIdx < vr::k_unMaxTrackedDeviceCount; ++deviceIdx) {
		if (!devicePose[deviceIdx].bPoseIsValid) {
			continue;
		}
		devicePoseMat[deviceIdx] = steamMatToMatrix4(devicePose[deviceIdx].mDeviceToAbsoluteTracking);
		if (classForDeviceIdx[deviceIdx] != 0) {
			//continue;
		}
		if (hmd->GetTrackedDeviceClass(deviceIdx) == vr::TrackedDeviceClass_Controller) {
			if (hand == 0) {
				rightHandPose = devicePoseMat[deviceIdx];
			}
			if (hand == 1) {
				leftHandPose = devicePoseMat[deviceIdx];
			}
			hand++;
		}
		switch (hmd->GetTrackedDeviceClass(deviceIdx)) {
			case vr::TrackedDeviceClass_Controller:        classForDeviceIdx[deviceIdx] = 'C'; break;
			case vr::TrackedDeviceClass_HMD:               classForDeviceIdx[deviceIdx] = 'H'; break;
			case vr::TrackedDeviceClass_Invalid:           classForDeviceIdx[deviceIdx] = 'I'; break;
			case vr::TrackedDeviceClass_GenericTracker:    classForDeviceIdx[deviceIdx] = 'G'; break;
			case vr::TrackedDeviceClass_TrackingReference: classForDeviceIdx[deviceIdx] = 'T'; break;
			default:                                       classForDeviceIdx[deviceIdx] = '?'; break;
		}
		poseClasses += classForDeviceIdx[deviceIdx];
	}

	if (devicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
		hmdPose = devicePoseMat[vr::k_unTrackedDeviceIndex_Hmd];
		inverseHmdPose = hmdPose;
		inverseHmdPose.invert();
	}
}

// --------------------------------- init ------------------------------------
bool App::init() {


	// SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
		printf("%s - SDL could not initialize! SDL Error: %s\n", __FUNCTION__, SDL_GetError());
		return false;
	}

	// Loading the SteamVR Runtime
	vr::EVRInitError eError = vr::VRInitError_None;
	hmd = vr::VR_Init(&eError, vr::VRApplication_Scene);
	if (eError != vr::VRInitError_None) {
		hmd = NULL;
		char buf[1024];
		sprintf_s(buf, sizeof(buf), "Unable to init VR runtime: %s", vr::VR_GetVRInitErrorAsEnglishDescription(eError));
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VR_Init Failed", buf, NULL);
		return false;
	}

	// Setup SDL & Monitor window
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

	int windowPosX = 700;
	int windowPosY = 100;
	Uint32 unWindowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
	monitorWindow = SDL_CreateWindow("hellovr", windowPosX, windowPosY, monitorWinWidth, monitorWinHeight, unWindowFlags);
	if (monitorWindow == NULL) {
		printf("%s - Window could not be created! SDL Error: %s\n", __FUNCTION__, SDL_GetError());
		return false;
	}
	monitorGlContext = SDL_GL_CreateContext(monitorWindow);
	if (monitorGlContext == NULL) {
		printf("%s - OpenGL context could not be created! SDL Error: %s\n", __FUNCTION__, SDL_GetError());
		return false;
	}

	// Setup glew
	glewExperimental = GL_TRUE;
	GLenum glewError = glewInit();
	if (glewError != GLEW_OK) {
		printf("%s - Error initializing GLEW! %s\n", __FUNCTION__, glewGetErrorString(glewError));
		return false;
	}
	glGetError(); // to clear the error caused deep in GLEW

	// cube array
	brickTextureId = 0;
	sceneVertCount = 0;

	if (!initGl()) {
		printf("%s - Unable to initialize OpenGL!\n", __FUNCTION__);
		return false;
	}

	if (!vr::VRCompositor()) {
		printf("Compositor initialization failed. See log file for details\n", __FUNCTION__);
		return false;
	}

	return true;
}

std::string App::getDeviceString(vr::IVRSystem *hmd, vr::TrackedDeviceIndex_t deviceIdx, vr::TrackedDeviceProperty deviceProp, vr::TrackedPropertyError *err) {
	uint32_t buffLen = hmd->GetStringTrackedDeviceProperty(deviceIdx, deviceProp, NULL, 0, err);
	if (buffLen == 0)
		return "";

	char *buff = new char[buffLen];
	buffLen = hmd->GetStringTrackedDeviceProperty(deviceIdx, deviceProp, buff, buffLen, err);
	std::string sResult = buff;
	delete[] buff;
	return sResult;
}

bool App::initGl() {
	if (!createShaders())
		return false;

	initFonts();
	loadTextures();
	setupCameraMatrices();
	setupHmdRenderTargets();
	setupMonitorWindow();
	initDeviceModels();
	  
	return true;
}

void App::initFonts() {
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1); // Disable byte-alignment restriction

	FT_Library ft;
	FT_Face face;
	std::string prefix("../assets/");
	std::string pwd = Path_StripFilename(Path_GetExecutablePath());
	std::string fontPath = Path_MakeAbsolute(prefix + "open-sans/OpenSans-Regular.ttf", pwd);
	if (FT_Init_FreeType(&ft))
		std::cout << "ERROR::FREETYPE: Could not init FreeType Library" << std::endl;
	if (FT_New_Face(ft, fontPath.c_str(), 0, &face))
		std::cout << "ERROR::FREETYPE: Failed to load font" << std::endl;
	FT_Set_Pixel_Sizes(face, 0, 48);
	if (FT_Load_Char(face, 'X', FT_LOAD_RENDER))
		std::cout << "ERROR::FREETYTPE: Failed to load Glyph" << std::endl;

	for (GLubyte c = 0; c < 128; c++) {
		// Load character glyph 
		if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
			std::cout << "ERROR::FREETYTPE: Failed to load Glyph" << std::endl;
			continue;
		}

		// Generate texture
		GLuint texture;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(
			GL_TEXTURE_2D,
			0,
			GL_RED,
			face->glyph->bitmap.width,
			face->glyph->bitmap.rows,
			0,
			GL_RED,
			GL_UNSIGNED_BYTE,
			face->glyph->bitmap.buffer
		);

		// Set texture options
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// Now store character for later use
		Character character = {
			texture,
			glm::ivec2(face->glyph->bitmap.width, face->glyph->bitmap.rows),
			glm::ivec2(face->glyph->bitmap_left, face->glyph->bitmap_top),
			face->glyph->advance.x
		};
		Characters.insert(std::pair<GLchar, Character>(c, character));
	}
	FT_Done_Face(face);
	FT_Done_FreeType(ft);

	glGenVertexArrays(1, &VAO);
	glGenBuffers(1, &VBO);
	glBindVertexArray(VAO);
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 6 * 4, NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
}

GLuint App::loadShader(std::string name) {
	std::string prefix("../assets/");
	std::string pwd = Path_StripFilename(Path_GetExecutablePath());
	std::string vertPath = Path_MakeAbsolute(prefix + name + "Shader.vert", pwd);
	std::string fragPath = Path_MakeAbsolute(prefix + name + "Shader.frag", pwd);
	std::string sceneVert((std::istreambuf_iterator<char>(std::ifstream(vertPath))), std::istreambuf_iterator<char>());
	std::string sceneFrag((std::istreambuf_iterator<char>(std::ifstream(fragPath))), std::istreambuf_iterator<char>());

	GLuint id = glutil::compileShader(name.c_str(), sceneVert.c_str(), sceneFrag.c_str());

	return id;
}

bool App::createShaders()
{
	sceneShader = loadShader("Scene");
	sceneShaderMatrix = glGetUniformLocation(sceneShader, "matrix");
	if (sceneShaderMatrix == -1) {
		printf("Unable to find matrix uniform in scene shader\n");
		return false;
	}

	controllerShader = loadShader("Controller");
	controllerShaderMatrix = glGetUniformLocation(controllerShader, "matrix");
	if (controllerShaderMatrix == -1) {
		printf("Unable to find matrix uniform in controller shader\n");
		return false;
	}

	renderModelShader = loadShader("RenderModel");
	renderModelShaderMatrix = glGetUniformLocation(renderModelShader, "matrix");
	if (renderModelShaderMatrix == -1) {
		printf("Unable to find matrix uniform in render model shader\n");
		return false;
	}

	fontShader = loadShader("Font");
	textColor = glGetUniformLocation(fontShader, "textColor");
	textProj = glGetUniformLocation(fontShader, "projection");
	if (textColor == -1 || textProj == -1) {
		printf("Unable to find uniform in shader\n");
		return false;
	}

	monitorWindowShader = loadShader("MonitorWindow");

	return sceneShader != 0
		&& controllerShader != 0
		&& renderModelShader != 0
		&& monitorWindowShader != 0;
}

bool App::loadTextures() {
	std::string pwd = Path_StripFilename(Path_GetExecutablePath());

	brickTextureId = glutil::loadTexture(Path_MakeAbsolute("../assets/brick.png", pwd));

	return (brickTextureId != 0);
}

void App::setupCameraMatrices() {
	leftEyeProj = getEyeProj(vr::Eye_Left);
	rightEyeProj = getEyeProj(vr::Eye_Right);
	leftEyePos = getEyePos(vr::Eye_Left);
	rightEyePos = getEyePos(vr::Eye_Right);
}

Matrix4 App::getEyeProj(vr::Hmd_Eye nEye) {
	if (!hmd)
		return Matrix4();

	float nearClip = 0.1f;
	float farClip = 30.0f;
	vr::HmdMatrix44_t mat = hmd->GetProjectionMatrix(nEye, nearClip, farClip);

	return Matrix4(
		mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
		mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
		mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
		mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]
	);
}

Matrix4 App::getEyePos(vr::Hmd_Eye nEye) {
	if (!hmd)
		return Matrix4();

	vr::HmdMatrix34_t matEyeRight = hmd->GetEyeToHeadTransform(nEye);
	Matrix4 matrixObj(
		matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
		matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
		matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
		matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f
	);

	return matrixObj.invert();
}

bool App::setupHmdRenderTargets() {
	if (!hmd)
		return false;

	hmd->GetRecommendedRenderTargetSize(&hmdRenderWidth, &hmdRenderHeight);

	createFrameBuffer(hmdRenderWidth, hmdRenderHeight, leftEyeDesc);
	createFrameBuffer(hmdRenderWidth, hmdRenderHeight, rightEyeDesc);

	return true;
}

void App::setupMonitorWindow() {
	if (!hmd)
		return;

	std::vector<VertexDataWindow> verts;

	// left eye verts
	verts.push_back(VertexDataWindow(Vector2(-1, -1), Vector2(0, 1)));
	verts.push_back(VertexDataWindow(Vector2(0, -1), Vector2(1, 1)));
	verts.push_back(VertexDataWindow(Vector2(-1, 1), Vector2(0, 0)));
	verts.push_back(VertexDataWindow(Vector2(0, 1), Vector2(1, 0)));

	// right eye verts
	verts.push_back(VertexDataWindow(Vector2(0, -1), Vector2(0, 1)));
	verts.push_back(VertexDataWindow(Vector2(1, -1), Vector2(1, 1)));
	verts.push_back(VertexDataWindow(Vector2(0, 1), Vector2(0, 0)));
	verts.push_back(VertexDataWindow(Vector2(1, 1), Vector2(1, 0)));

	GLushort indices[] = { 0, 1, 3,   0, 3, 2,   4, 5, 7,   4, 7, 6 };
	monitorWinIdxSize = _countof(indices);

	glGenVertexArrays(1, &monitorWinVertAr);
	glBindVertexArray(monitorWinVertAr);

	glGenBuffers(1, &monitorWinVertBuff);
	glBindBuffer(GL_ARRAY_BUFFER, monitorWinVertBuff);
	glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(VertexDataWindow), &verts[0], GL_STATIC_DRAW);

	glGenBuffers(1, &monitorWinIdxBuff);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, monitorWinIdxBuff);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, monitorWinIdxSize * sizeof(GLushort), &indices[0], GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataWindow), (void *)offsetof(VertexDataWindow, position));

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexDataWindow), (void *)offsetof(VertexDataWindow, texCoord));

	glBindVertexArray(0);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void App::initDeviceModels() {
	memset(trackedDeviceModels, 0, sizeof(trackedDeviceModels));
	if (!hmd)
		return;

	for (uint32_t deviceIdx = vr::k_unTrackedDeviceIndex_Hmd + 1; deviceIdx < vr::k_unMaxTrackedDeviceCount; deviceIdx++)	{
		if (!hmd->IsTrackedDeviceConnected(deviceIdx))
			continue;
		initDeviceModel(deviceIdx);
	}
}

void App::initDeviceModel(vr::TrackedDeviceIndex_t deviceIdx) {
	if (deviceIdx >= vr::k_unMaxTrackedDeviceCount)
		return;

	// try to find a model we've already set up
	std::string modelName = getDeviceString(hmd, deviceIdx, vr::Prop_RenderModelName_String);
	CGLRenderModel *renderModel = getDeviceModel(modelName.c_str());
	if (!renderModel) {
		std::string sTrackingSystemName = getDeviceString(hmd, deviceIdx, vr::Prop_TrackingSystemName_String);
		printf("Unable to load render model for tracked device %d (%s.%s)", deviceIdx, sTrackingSystemName.c_str(), modelName.c_str());
	} else {
		trackedDeviceModels[deviceIdx] = renderModel;
	}
}

CGLRenderModel *App::getDeviceModel(const char *modelName) {

	// Find model
	CGLRenderModel *renderModel = NULL;
	for (std::vector< CGLRenderModel * >::iterator i = modelInventory.begin(); i != modelInventory.end(); i++) {
		if (!stricmp((*i)->GetName().c_str(), modelName)) {
			renderModel = *i;
			break;
		}
	}
	if(renderModel) {
		return renderModel;
	}

	// Load model
	vr::RenderModel_t *model;
	vr::EVRRenderModelError error;
	while (1) {
		error = vr::VRRenderModels()->LoadRenderModel_Async(modelName, &model);
		if (error != vr::VRRenderModelError_Loading)
			break;
		sleep(1);
	}
	if (error != vr::VRRenderModelError_None) {
		printf("Unable to load render model %s - %s\n", modelName, vr::VRRenderModels()->GetRenderModelErrorNameFromEnum(error));
		return NULL; // move on to the next tracked device
	}

	// Load texture
	vr::RenderModel_TextureMap_t *texture;
	while (1) {
		error = vr::VRRenderModels()->LoadTexture_Async(model->diffuseTextureId, &texture);
		if (error != vr::VRRenderModelError_Loading)
			break;
		sleep(1);
	}
	if (error != vr::VRRenderModelError_None) {
		printf("Unable to load render texture id:%d for render model %s\n", model->diffuseTextureId, modelName);
		vr::VRRenderModels()->FreeRenderModel(model);
		return NULL; // move on to the next tracked device
	}

	// Generate render model
	renderModel = new CGLRenderModel(modelName);
	if (!renderModel->BInit(*model, *texture)) {
		printf("Unable to create GL model from render model %s\n", modelName);
		delete renderModel;
		renderModel = NULL;
	} else {
		modelInventory.push_back(renderModel);
	}
	vr::VRRenderModels()->FreeRenderModel(model);
	vr::VRRenderModels()->FreeTexture(texture);

	return renderModel;
}

// --------------------------------- ???? ------------------------------------
void App::sleep( unsigned long millis )
{
#if defined(_WIN32)
	::Sleep( millis );
#elif defined(POSIX)
	usleep(millis * 1000 );
#endif
}

Matrix4 App::steamMatToMatrix4( const vr::HmdMatrix34_t &matPose ) {
	Matrix4 matrixObj(
		matPose.m[0][0], matPose.m[1][0], matPose.m[2][0], 0.0,
		matPose.m[0][1], matPose.m[1][1], matPose.m[2][1], 0.0,
		matPose.m[0][2], matPose.m[1][2], matPose.m[2][2], 0.0,
		matPose.m[0][3], matPose.m[1][3], matPose.m[2][3], 1.0f
		);
	return matrixObj;
}

void App::shutdown() {
	if( hmd ) {
		vr::VR_Shutdown();
		hmd = NULL;
	}

	for( std::vector< CGLRenderModel * >::iterator i = modelInventory.begin(); i != modelInventory.end(); i++ ) {
		delete (*i);
	}
	modelInventory.clear();
	
	if( monitorGlContext ) {
		glDebugMessageControl( GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_FALSE );
		glDebugMessageCallback(nullptr, nullptr);
		
		if(sceneVertBuffer) {
			glDeleteBuffers(1, &sceneVertBuffer);
			sceneVertBuffer = 0;
		}

		if ( sceneShader )
			glDeleteProgram( sceneShader );
		if ( controllerShader )
			glDeleteProgram( controllerShader );
		if ( renderModelShader )
			glDeleteProgram( renderModelShader );
		if ( monitorWindowShader )
			glDeleteProgram( monitorWindowShader );

		glDeleteRenderbuffers( 1, &leftEyeDesc.depthBufferId );
		glDeleteTextures( 1, &leftEyeDesc.renderTextureId );
		glDeleteFramebuffers( 1, &leftEyeDesc.renderFramebufferId );
		glDeleteTextures( 1, &leftEyeDesc.resolveTextureId );
		glDeleteFramebuffers( 1, &leftEyeDesc.resolveFramebufferId );

		glDeleteRenderbuffers( 1, &rightEyeDesc.depthBufferId );
		glDeleteTextures( 1, &rightEyeDesc.renderTextureId );
		glDeleteFramebuffers( 1, &rightEyeDesc.renderFramebufferId );
		glDeleteTextures( 1, &rightEyeDesc.resolveTextureId );
		glDeleteFramebuffers( 1, &rightEyeDesc.resolveFramebufferId );

		if( monitorWinVertAr != 0 )
			glDeleteVertexArrays( 1, &monitorWinVertAr );
		if( sceneVertexAr != 0 ) {
			glDeleteVertexArrays( 1, &sceneVertexAr );
			sceneVertexAr = 0;
		}
		if( controllerVertAr != 0 )
			glDeleteVertexArrays( 1, &controllerVertAr );
	}

	if( monitorWindow )	{
		SDL_DestroyWindow(monitorWindow);
		monitorWindow = NULL;
	}

	SDL_Quit();
}
