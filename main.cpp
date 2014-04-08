#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include <iostream>
#include <Windows.h>
#include <process.h>
#include "opencv2/core/opengl_interop.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "maths_funcs.h"

#include <GL/glew.h>
#include <GL/freeglut.h>

#include "OpenNI.h"



using namespace std;
using namespace cv;

VideoCapture cap;
Mat frame;
Mat perspective_warped_image;
Point2f  src_pts[4], dst_pts[4];
GLuint shaderProgramID;
unsigned char* buffer;
GLfloat vertices[108];
GLuint numVertices;
GLuint bgShader;
GLuint VBOQ; // VBO for the Quad
GLuint VBO; // vbo for the cube
int endOfColours,endOfQuad;
int selected_cube;
float scalingFactor = 0.008f;

float KDFX = 5.9421434211923247e+02;
float KDFY = 5.9104053696870778e+02;
float KDCX = 3.3930780975300314e+02;
float KDCY = 2.4273913761751615e+02;
float zNear = 0.1;
float zFar = 500;

// camera matrix for the kinect
mat4 DepthCameraProj = mat4(2*KDFX/640,	0,	1-2*KDCX/640,	0,
						  0,	 2*KDFY/480	,-1+(2*KDCY+2)/480,	0,
						  0,	0,	(zNear+zFar)/(zNear - zFar),	2*zNear*zFar/(zNear - zFar),
						  0,	0,	-1,	0);
mat4 DepthCameraMat = mat4(-KDFX,0,-KDCX,0,
						   0,-KDFY,-KDCY,0,
						   0,0,1,0,
						   0,0,0,0);
Mat m_depthImage, m_colorImage, rgb_image;
openni::VideoFrameRef m_depthFrame, m_colorFrame;
openni::VideoStream m_depth, m_color;
openni::Device m_device;


vector<vec3> cubes;
//Calibration variables
bool calibrateZ = false;
double markerZvalue = -1;
double baseRadius = 0;
bool calibrated = false;
bool grabbed = false;
bool hasInitialized = false;

Size patternsize(7,7); //interior number of corners
Mat cameraMatrix = Mat::eye(3, 3, CV_64F);
Mat distCoeffs = Mat::zeros(8, 1, CV_64F);
vector<Mat> rvecs, tvecs;
vector<vector<Point2f>> imagePoints;
vector<vector<Point3f>> objectPoints;
Mat projection = Mat::zeros(4, 4, CV_64F);;
Mat modelview = Mat::zeros(4, 4, CV_64F);;
Mat openGLtoCV;
mat4 modelV_mat4, view, persp_proj;
vec3 worldPos, closestPoint, grabbed_vertex, start, endPos;
vec3 pointerLocHomogenous;
int testImages = 0;


int closest_depth_value;
Point closest_depth_point = Point(1,1);

// file for the shaders to write to
FILE *ErrorTxt;


float* getTransform(Mat& mat);

vec3 getClosest(Point pointerLoc);
float getDist(vec3 point, vec3 otherPoint);

Point object_pt;
bool found=false;
Mat object_mat;
Rect obj;
int radius;
Mat mask;

void thread(void* );
void chessboard_thread(void* );
GLfloat* modelV;
GLfloat* projV;

GLuint bg_tex; // texture that the open cv frame gets put into 

// Macro for indexing vertex buffer
#define BUFFER_OFFSET(i) ((char *)NULL + (i))

using namespace std;


#pragma region SHADER_FUNCTIONS
char* readShaderSource(const char* shaderFile) {   
    FILE* fp = fopen(shaderFile, "rb"); //!->Why does binary flag "RB" work and not "R"... wierd msvc thing?

    if ( fp == NULL ) { return NULL; }

    fseek(fp, 0L, SEEK_END);
    long size = ftell(fp);

    fseek(fp, 0L, SEEK_SET);
    char* buf = new char[size + 1];
    fread(buf, 1, size, fp);
    buf[size] = '\0';

    fclose(fp);

    return buf;
}


static void AddShader(GLuint ShaderProgram, const char* pShaderText, GLenum ShaderType)
{
	// create a shader object
    GLuint ShaderObj = glCreateShader(ShaderType);

    if (ShaderObj == 0) {
        fprintf(ErrorTxt, "Error creating shader type %d\n", ShaderType);
        exit(0);
    }
	const char* pShaderSource = readShaderSource( pShaderText);
	// Bind the source code to the shader, this happens before compilation
	glShaderSource(ShaderObj, 1, (const GLchar**)&pShaderSource, NULL);
	// compile the shader and check for errors
    glCompileShader(ShaderObj);
    GLint success;
	// check for shader related errors using glGetShaderiv
    glGetShaderiv(ShaderObj, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar InfoLog[1024];
        glGetShaderInfoLog(ShaderObj, 1024, NULL, InfoLog);
        fprintf(ErrorTxt, "Error compiling shader type %d: '%s'\n", ShaderType, InfoLog);
        exit(1);
    }
	// Attach the compiled shader object to the program object
    glAttachShader(ShaderProgram, ShaderObj);
}

GLuint CompileShaders()
{
	//Start the process of setting up our shaders by creating a program ID
	//Note: we will link all the shaders together into this ID
    shaderProgramID = glCreateProgram();
    if (shaderProgramID == 0) {
        fprintf(ErrorTxt, "Error creating shader program\n");
        exit(1);
    }

	// Create two shader objects, one for the vertex, and one for the fragment shader
    AddShader(shaderProgramID, "../Shaders/simpleVertexShader.txt", GL_VERTEX_SHADER);
    AddShader(shaderProgramID, "../Shaders/simpleFragmentShader.txt", GL_FRAGMENT_SHADER);
    GLint Success = 0;
    GLchar ErrorLog[1024] = { 0 };
	// After compiling all shader objects and attaching them to the program, we can finally link it
    glLinkProgram(shaderProgramID);
	// check for program related errors using glGetProgramiv
    glGetProgramiv(shaderProgramID, GL_LINK_STATUS, &Success);
	if (Success == 0) {
		glGetProgramInfoLog(shaderProgramID, sizeof(ErrorLog), NULL, ErrorLog);
		fprintf(ErrorTxt, "Error linking shader program: '%s'\n", ErrorLog);
        exit(1);
	}

	// program has been successfully linked but needs to be validated to check whether the program can execute given the current pipeline state
    glValidateProgram(shaderProgramID);
	// check for program related errors using glGetProgramiv
    glGetProgramiv(shaderProgramID, GL_VALIDATE_STATUS, &Success);
    if (!Success) {
        glGetProgramInfoLog(shaderProgramID, sizeof(ErrorLog), NULL, ErrorLog);
        fprintf(ErrorTxt, "Invalid shader program: '%s'\n", ErrorLog);
        exit(1);
    }
	// Finally, use the linked shader program
	// Note: this program will stay in effect for all draw calls until you replace it with another or explicitly disable its use
    glUseProgram(shaderProgramID);
	return shaderProgramID;
}

GLuint compileQuadShaders()
{
	bgShader = glCreateProgram();
    if (bgShader == 0) {
        fprintf(ErrorTxt, "Error creating shader program\n");
        exit(1);
    }

	// Create two shader objects, one for the vertex, and one for the fragment shader
    AddShader(bgShader, "../Shaders/bgVertShader.txt", GL_VERTEX_SHADER);
    AddShader(bgShader, "../Shaders/bgFragShader.txt", GL_FRAGMENT_SHADER);

    GLint Success = 0;
    GLchar ErrorLog[1024] = { 0 };
	// After compiling all shader objects and attaching them to the program, we can finally link it
    glLinkProgram(bgShader);
	// check for program related errors using glGetProgramiv
    glGetProgramiv(bgShader, GL_LINK_STATUS, &Success);
	if (Success == 0) {
		glGetProgramInfoLog(bgShader, sizeof(ErrorLog), NULL, ErrorLog);
		fprintf(ErrorTxt, "Error linking shader program: '%s'\n", ErrorLog);
        exit(1);
	}

	// program has been successfully linked but needs to be validated to check whether the program can execute given the current pipeline state
    glValidateProgram(bgShader);
	// check for program related errors using glGetProgramiv
    glGetProgramiv(bgShader, GL_VALIDATE_STATUS, &Success);
    if (!Success) {
        glGetProgramInfoLog(bgShader, sizeof(ErrorLog), NULL, ErrorLog);
        fprintf(ErrorTxt, "Invalid shader program: '%s'\n", ErrorLog);
        exit(1);
    }
	// Finally, use the linked shader program
	// Note: this program will stay in effect for all draw calls until you replace it with another or explicitly disable its use
    glUseProgram(bgShader);
	return bgShader;
}

#pragma endregion SHADER_FUNCTIONS

#pragma region VBO_FUNCTIONS
GLuint generateObjectBuffer(GLfloat vertices[], GLfloat colors[], GLfloat Quad[], GLfloat Quadtc[]) {
	numVertices = 36;
	int quadSize = 6;
	int nextStart;
	// Genderate 1 generic buffer object, called VBO
 	glGenBuffers(1, &VBO);
	// In OpenGL, we bind (make active) the handle to a target name and then execute commands on that target
	// Buffer will contain an array of vertices 
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	// After binding, we now fill our object with data, everything in "Vertices" goes to the GPU
	glBufferData(GL_ARRAY_BUFFER, numVertices*7*sizeof(GLfloat), NULL, GL_STREAM_DRAW);
	// if you have more data besides vertices (e.g., vertex colours or normals), use glBufferSubData to tell the buffer when the vertices array ends and when the colors start
	glBufferSubData (GL_ARRAY_BUFFER, 0, numVertices*3*sizeof(GLfloat), vertices);
	nextStart = numVertices*3*sizeof(GLfloat);
	glBufferSubData (GL_ARRAY_BUFFER,nextStart, numVertices*4*sizeof(GLfloat), colors);
	nextStart += numVertices*4*sizeof(GLfloat);
	endOfColours = nextStart;
	glBufferSubData(GL_ARRAY_BUFFER, nextStart, quadSize*3*sizeof(GLfloat),Quad);
	nextStart += quadSize*3*sizeof(GLfloat);
	endOfQuad = nextStart; 
	glBufferSubData(GL_ARRAY_BUFFER,nextStart, quadSize*2*sizeof(GLfloat),Quadtc);
return VBO;
}

void linkCurrentBuffertoShader(GLuint shaderProgramID){
	GLuint numVertices = 36;
	// find the location of the variables that we will be using in the shader program
	GLuint positionID = glGetAttribLocation(shaderProgramID, "vPosition");
	GLuint colorID = glGetAttribLocation(shaderProgramID, "vColor");
	// Have to enable this
	glEnableVertexAttribArray(positionID);
	// Tell it where to find the position data in the currently active buffer (at index positionID)
    glVertexAttribPointer(positionID, 3, GL_FLOAT, GL_FALSE, 0, 0);
	// Similarly, for the color data.
	glEnableVertexAttribArray(colorID);
	glVertexAttribPointer(colorID, 4, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(numVertices*3*sizeof(GLfloat)));
/*
	glUseProgram(bgShader);
	GLuint positionLoc = glGetAttribLocation(bgShader, "vPosition");
	GLuint tcLoc = glGetAttribLocation(bgShader, "tc");
	glEnableVertexAttribArray(positionLoc);
	glVertexAttribPointer(positionLoc,3,GL_FLOAT,GL_FALSE,0,BUFFER_OFFSET(endOfColours));
	glEnableVertexAttribArray(tcLoc);
	glVertexAttribPointer(tcLoc,2,GL_FLOAT,GL_FALSE,0,BUFFER_OFFSET(endOfQuad));
	*/
}
GLuint generateQuadObjectBuffer(GLfloat vertices[], GLfloat tex[]) {
	numVertices = 6;
	// Genderate 1 generic buffer object, called VBO
	
 	glGenBuffers(1, &VBOQ);
	// In OpenGL, we bind (make active) the handle to a target name and then execute commands on that target
	// Buffer will contain an array of vertices 
	glBindBuffer(GL_ARRAY_BUFFER, VBOQ);
	// After binding, we now fill our object with data, everything in "Vertices" goes to the GPU
	glBufferData(GL_ARRAY_BUFFER, numVertices*7*sizeof(GLfloat), NULL, GL_STREAM_DRAW);
	glBufferSubData (GL_ARRAY_BUFFER, 0, numVertices*3*sizeof(GLfloat), vertices);
	glBufferSubData (GL_ARRAY_BUFFER, numVertices*3*sizeof(GLfloat), numVertices*2*sizeof(GLfloat), tex);
return VBOQ;
}

void linkQuadBuffertoShader(GLuint shaderProgramID){
	GLuint numVertices = 6;
	// find the location of the variables that we will be using in the shader program
	GLuint positionID = glGetAttribLocation(shaderProgramID, "vPosition");
	GLuint texID = glGetAttribLocation(shaderProgramID, "tc");
	// Have to enable this
	glEnableVertexAttribArray(positionID);
	// Tell it where to find the position data in the currently active buffer (at index positionID)
    glVertexAttribPointer(positionID, 3, GL_FLOAT, GL_FALSE, 0, 0);
	// Similarly, for the color data.
	glEnableVertexAttribArray(texID);
	glVertexAttribPointer(texID, 4, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(numVertices*3*sizeof(GLfloat)));
}
#pragma endregion VBO_FUNCTIONS

void translateVertex (vec3 vertex, vec3 vector){
	for (int i=0; i<36; i++){
		//edit the relevant vertices in the vertex array
		if (vertices[3*i] == vertex.v[0] && vertices[3*i+1] == vertex.v[1] && vertices[3*i+2] == vertex.v[2]){
					vertices[3*i] += vector.v[0];
					vertices[3*i+1] += vector.v[1];
					vertices[3*i+2] += vector.v[2];
		}
		//reload the vertex buffer
		glBufferSubData (GL_ARRAY_BUFFER, 0, numVertices*3*sizeof(GLfloat), vertices);
	}
}

vec3 convertToModelCoords(vec3 worldcoords){
	//make a mat4 version of the modelview matrix
	//this is now done in the chessboard function
	//then use that to create the model matrix of the untranslated cube
	mat4 model = modelV_mat4*scale(identity_mat4(), vec3(0.25, 0.25, 0.25));
	
	//derive inverse of the matrix
	mat4 model_inv = inverse(model);

	//undo the effects of those darn matrices
	vec4 vec4_worldcoords = vec4(worldcoords, 1);
	vec3 result = model_inv*vec4_worldcoords;

	return result;
}


int getClosestCube(){
	//find closest centre
	int closest = 0; 
	vec3 current;
	float currentDist;
	float closestDist = 500;
	for (int i=0; i<cubes.size(); i++)
	{
		currentDist = getDist(worldPos,cubes[i]);
		if(currentDist < closestDist)
		{
			closestDist = currentDist;
			closest = i; 
		}
	}

	return closest;
}

void drawALLTheCubes(mat4 original){
	int model_mat_location = glGetUniformLocation (shaderProgramID, "model");
	int selected_loc = glGetUniformLocation(shaderProgramID, "selected");
	selected_cube = getClosestCube();
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	for (int i=0; i<cubes.size(); i++){
		mat4 model = translate(identity_mat4(), cubes[i]);
		model = original*scale(model, vec3(0.25, 0.25, 0.25));
		if(i == selected_cube)
		{
			glUniform1i(selected_loc, 1);
		}
		else 
		{
			glUniform1i(selected_loc, 0 );
		}
		glUniformMatrix4fv (model_mat_location, 1, GL_FALSE, model.m);
		glDrawArrays(GL_TRIANGLES, 0, 36);
	}
	mat4 model = translate(scale(identity_mat4(), vec3(0.25, 0.25, 0.25)), worldPos);
	model = original*scale(model, vec3(0.25, 0.25, 0.25));
	glUniformMatrix4fv (model_mat_location, 1, GL_FALSE, model.m);
	glDrawArrays(GL_TRIANGLES, 0, 36);
	glDisable(GL_BLEND);
}

void getKinectFrames()
{

	m_depth.readFrame(&m_depthFrame);
	m_depthImage.create(m_depthFrame.getHeight(), m_depthFrame.getWidth(), CV_16UC1);
	m_depthImage.data = (uchar*)m_depthFrame.getData();
	m_depthImage.at<short>(closest_depth_point.y,closest_depth_point.x) = 32767;

	m_color.readFrame(&m_colorFrame);
	m_colorImage.create(m_colorFrame.getHeight(), m_colorFrame.getWidth(), CV_8UC3);
	m_colorImage.data = (uchar*)m_colorFrame.getData();
	cvtColor(m_colorImage, rgb_image, CV_BGR2RGB);
	frame = rgb_image.clone();
}
// gets the point closest to the kinect from the depth map
void getDepthClosestPoint(Mat depthMap)
{

	Point closest_point;
	short closest_value = 32767;
	short current ;
	for(int cols = 0; cols < depthMap.cols; cols++)
	{
		for(int rows = 0; rows < depthMap.rows; rows++)
		{
			current = depthMap.at<short>(rows,cols);
			if(current < closest_value && current > 0)
			{
				closest_point = Point(cols,rows);
				closest_value = current;
			}
		}
	}

	closest_depth_point = closest_point;
	closest_depth_value = closest_value;
}
void display(){

	getKinectFrames();
	getDepthClosestPoint(m_depthImage);
	getClosest(closest_depth_point);

	//cout << "this is the closest point ";
	//print(temp);
	if (grabbed){
		endPos = worldPos;
		vec3 translation =  endPos - start;
		translateVertex(grabbed_vertex, translation);
		grabbed_vertex += translation;
		start = endPos;
	}
	//perspective_warped_image = Mat::zeros(frame.rows, frame.cols, CV_8UC3);
	flip(frame, frame, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// transfer contents of the cv:Mat to the texture

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,frame.cols,frame.rows,0,GL_BGR,GL_UNSIGNED_BYTE,frame.ptr());

	// draw the quad
	glUseProgram(bgShader);
	glBindTexture(GL_TEXTURE_2D,bg_tex);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glClear(GL_DEPTH_BUFFER_BIT);

	// rebind the cube vbo and shaders 

	glUseProgram(shaderProgramID);
	// draw the cube 
	int proj_mat_location = glGetUniformLocation (shaderProgramID, "proj");
	int model_mat_location = glGetUniformLocation (shaderProgramID, "model");

	mat4 translations = translate(identity_mat4(),vec3(0,-1,-5));
	
	
	glUniformMatrix4fv (model_mat_location, 1, GL_FALSE, modelV_mat4.m);
	glUniformMatrix4fv (proj_mat_location, 1, GL_FALSE, DepthCameraProj.m);
	

	int selected_loc = glGetUniformLocation(shaderProgramID, "selected");

	drawALLTheCubes(modelV_mat4);
	glutSwapBuffers();
		

	glutPostRedisplay();
}

void init()
{	
#pragma region vertices 
	GLfloat temp[] = {  
							-1.0f,  1.0f, -1.0f, 
							1.0f,  1.0f, -1.0f, 
							1.0f, -1.0f, -1.0f, 
							1.0f, -1.0f, -1.0f,  
							-1.0f, -1.0f, -1.0f, 
							-1.0f,  1.0f, -1.0f, 

							-1.0f,  1.0f, 1.0f,  
							1.0f,  1.0f, 1.0f, 
							1.0f, -1.0f, 1.0f, 
							1.0f, -1.0f, 1.0f,  
							-1.0f, -1.0f, 1.0f, 
							-1.0f,  1.0f, 1.0f, 

							-1.0f,  1.0f, -1.0f,
							1.0f,  1.0f, -1.0f,
							1.0f,  1.0f, 1.0f, 
							1.0f,  1.0f, 1.0f,
							-1.0f,  1.0f, 1.0f, 
							-1.0f,  1.0f, -1.0f, 

							-1.0f,  -1.0f, -1.0f, 
							1.0f,  -1.0f, -1.0f, 
							1.0f,  -1.0f, 1.0f,
							1.0f,  -1.0f, 1.0f, 
							-1.0f,  -1.0f, 1.0f, 
							-1.0f,  -1.0f, -1.0f,

							-1.0f,   1.0f, 1.0f, 
							-1.0f,  -1.0f, 1.0f, 
							-1.0f,  -1.0f, -1.0f, 
							-1.0f,  -1.0f, -1.0f, 
							-1.0f,   1.0f, -1.0f, 
							-1.0f,   1.0f, 1.0f, 

							1.0f,   1.0f, 1.0f, 
							1.0f,  -1.0f, 1.0f, 
							1.0f,  -1.0f, -1.0f, 
							1.0f,  -1.0f, -1.0f, 
							1.0f,   1.0f, -1.0f, 
							1.0f,   1.0f, 1.0f
						};
	memcpy(vertices, temp, 108*sizeof(GLfloat)); 
	// Create a color array that identfies the colors of each vertex (format R, G, B, A)
	GLfloat colors[] = {1.0f, 0.0f, 1.0f, 1.0f,
						1.0f, 0.0f, 1.0f, 1.0f,
						1.0f, 0.0f, 1.0f, 1.0f,
						1.0f, 0.0f, 1.0f, 1.0f,
						1.0f, 0.0f, 1.0f, 1.0f,
						1.0f, 0.0f, 1.0f, 1.0f,

						1.0f, 0.0f, 0.0f, 1.0f,
						1.0f, 0.0f, 0.0f, 1.0f,
						1.0f, 0.0f, 0.0f, 1.0f,
						1.0f, 0.0f, 0.0f, 1.0f,
						1.0f, 0.0f, 0.0f, 1.0f,
						1.0f, 0.0f, 0.0f, 1.0f,

						0.0f, 0.0f, 1.0f, 1.0f,
						0.0f, 0.0f, 1.0f, 1.0f,
						0.0f, 0.0f, 1.0f, 1.0f,
						0.0f, 0.0f, 1.0f, 1.0f,
						0.0f, 0.0f, 1.0f, 1.0f,
						0.0f, 0.0f, 1.0f, 1.0f,

						0.0f, 1.0f, 0.0f, 1.0f,
						0.0f, 1.0f, 0.0f, 1.0f,
						0.0f, 1.0f, 0.0f, 1.0f,
						0.0f, 1.0f, 0.0f, 1.0f,
						0.0f, 1.0f, 0.0f, 1.0f,
						0.0f, 1.0f, 0.0f, 1.0f,

						1.0f, 1.0f, 0.0f, 1.0f,
						1.0f, 1.0f, 0.0f, 1.0f,
						1.0f, 1.0f, 0.0f, 1.0f,
						1.0f, 1.0f, 0.0f, 1.0f,
						1.0f, 1.0f, 0.0f, 1.0f,
						1.0f, 1.0f, 0.0f, 1.0f,

						0.0f, 1.0f, 1.0f, 1.0f,
						0.0f, 1.0f, 1.0f, 1.0f,
						0.0f, 1.0f, 1.0f, 1.0f,
						0.0f, 1.0f, 1.0f, 1.0f,
						0.0f, 1.0f, 1.0f, 1.0f,
						0.0f, 1.0f, 1.0f, 1.0f
	};

#pragma endregion vertices 

#pragma region quad
	
	GLfloat quad[] = 
	{
		-1.0f, -1.0f,0.0f,
		1.0f, -1.0f,0.0f,
		-1.0f,  1.0f,0.0f,

		-1.0f,  1.0f,0.0f,
		1.0f, -1.0f,0.0f,
		1.0f,  1.0f, 0.0f
	};

	GLfloat quad_tc[] = 
	{
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.0f, 1.0f,

		0.0, 1.0,
		1.0, 0.0,
		1.0, 1.0
	};


#pragma endregion quad 

	for (int x=0; x<4; x++){
		for (int y=0; y<4; y++){
			for (int z=0; z<4; z++){
				cubes.push_back(vec3(2*x, 2*y, 2*z));
			}
		}
	}

	// Set up the shaders
	shaderProgramID = CompileShaders();
	bgShader = compileQuadShaders();
	// Put the vertices and colors into a vertex buffer object
	generateObjectBuffer(vertices, colors,quad, quad_tc);
	// Link the current buffer to the shader
	linkCurrentBuffertoShader(shaderProgramID);	

	glGenTextures(1,&bg_tex);
	glBindTexture(GL_TEXTURE_2D, bg_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); 
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); 
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,frame.cols,frame.rows,0,GL_BGR,GL_UNSIGNED_BYTE,frame.ptr());

	glUseProgram (shaderProgramID);

	int proj_mat_location = glGetUniformLocation (shaderProgramID, "proj");
	int view_mat_location = glGetUniformLocation (shaderProgramID, "view");
	int model_mat_location = glGetUniformLocation (shaderProgramID, "model");


	persp_proj = perspective(50.0, (float)frame.cols/(float)frame.rows, 0.1, 500.0);
	view = identity_mat4();
	modelV_mat4 = rotate_x_deg(identity_mat4(), 30);
	modelV_mat4 = rotate_y_deg(modelV_mat4, 30);
	modelV_mat4 = translate(modelV_mat4,vec3(-1,0,-7));

	glUniformMatrix4fv (proj_mat_location, 1, GL_FALSE, persp_proj.m);
	glUniformMatrix4fv (view_mat_location, 1, GL_FALSE, view.m);
	glUniformMatrix4fv (model_mat_location, 1, GL_FALSE, modelV_mat4.m);

	// Enable depth test
	glEnable(GL_DEPTH_TEST);
	// Accept fragment if it closer to the camera than the former one
	glDepthFunc(GL_LESS);
}

void keypress(unsigned char key, int x, int y){
	
	/*if (key == 'g')
	{
		cout << "Grabbed is now true.\n";
		grabbed = true;
		grabbed_vertex = closestPoint;
		start = worldPos;
	}
	if (key == 's')
	{
		cout << "Grabbed is now false.\n";
		grabbed = false;
		endPos = worldPos;
		vec3 translation =  endPos - start;
		translateVertex(grabbed_vertex, translation);
		start = endPos;
	}*/
	if (key == 'd'){
		cout << "Deleting a cube.\n";
		cubes.erase(cubes.begin() + selected_cube);
	}
	if(key == 'c')
	{
		scalingFactor = closest_depth_value/-5;
		cout <<"this is the scaling factorv " <<  scalingFactor << endl;
	}
	if (key == 'y'){
		modelV_mat4 = modelV_mat4 * rotate_x_deg(identity_mat4(), 5) ;
	}
	if (key == 'h')
	{
		modelV_mat4 = modelV_mat4 *  rotate_x_deg(identity_mat4(), -5);
	}
	if (key == 'j')
	{
		modelV_mat4 = modelV_mat4 * rotate_y_deg(identity_mat4(), 5) ;
	}
	if (key == 'g')
	{
		modelV_mat4 = modelV_mat4 *  rotate_y_deg(identity_mat4(), -5);
	}
	if (key == 't')
	{
		modelV_mat4 = modelV_mat4 *  rotate_z_deg(identity_mat4(), 5);
	}
	if (key == 'u')
	{
		modelV_mat4 = modelV_mat4 *  rotate_z_deg(identity_mat4(), -5) ;
	}
	if (key == 'p')
	{
		modelV_mat4 = translate(identity_mat4(), pointerLocHomogenous);
	}
}

int main(int argc, char** argv)
{

	ErrorTxt = fopen("error.txt","w");

	bool m_status = openni::OpenNI::initialize();
	const char* deviceURI = openni::ANY_DEVICE;
	m_status = m_device.open(deviceURI);

	if(m_status != openni::STATUS_OK)
	{
		std::cout << "OpenCVKinect: Device open failseed: " << std::endl;
		std::cout << openni::OpenNI::getExtendedError() << std::endl;
		openni::OpenNI::shutdown();
		return false;
	}
	m_status = m_depth.create(m_device, openni::SENSOR_DEPTH);
	m_status = m_depth.start();
	m_status = m_color.create(m_device, openni::SENSOR_COLOR);
	m_status = m_color.start();

	m_depth.readFrame(&m_depthFrame);
	m_depthImage.create(m_depthFrame.getHeight(), m_depthFrame.getWidth(), CV_16UC1);
	m_depthImage.data = (uchar*)m_depthFrame.getData();
	m_color.readFrame(&m_colorFrame);
	m_colorImage.create(m_colorFrame.getHeight(), m_colorFrame.getWidth(), CV_8UC3);
	m_colorImage.data = (uchar*)m_colorFrame.getData();
	cvtColor(m_colorImage, rgb_image, CV_BGR2RGB);



	getDepthClosestPoint(m_depthImage);

	frame = m_colorImage.clone();
	mask = Mat(frame.size(), CV_8UC1);
	// Set up the window
	glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB);
    glutInitWindowSize((frame.cols*2), (frame.rows*2));
    glutCreateWindow("AR");
	// Tell glut where the display function is
	glutDisplayFunc(display);
	glutKeyboardFunc(keypress);

	 // A call to glewInit() must be done after glut is initialized!
    GLenum res = glewInit();
	// Check for any errors
    if (res != GLEW_OK) {
      fprintf(stderr, "Error: '%s'\n", glewGetErrorString(res));
      return 1;
    }
	// Set up your objects and shaders
	init();
	glViewport (0, 0, frame.cols, frame.rows);
	// Begin infinite event loop
	glutMainLoop();
	openni::OpenNI::shutdown();
    return 0;

}




/*
gets the closest vertex in the model to the cursor location
Point pointerLoc : the center of the tracking circle 
*/
vec3 getClosest(Point pointerLoc)
{
	// change the image coordinate to a homogenous vec3
	float Z, X, Y;

	Z = closest_depth_value/scalingFactor;

	//This takes the image coordinates of the green marker and converts them to world coordinates
	float x = pointerLoc.x - KDCX;
	float y = pointerLoc.y - KDCY;
	X = -(x*Z)/KDFX;
	Y = (y*Z)/KDFY;
	pointerLocHomogenous = vec3(X, Y, Z);

	// get the image point in world space 
	worldPos = convertToModelCoords(pointerLocHomogenous);

	//cout << "this is where it is in world space ";
	//print(worldPos);
	// find the closest vertex 
	int closest = 0; 
	vec3 current;
	float currentDist;
	float closestDist = 500;
	for (int i=0; i<36; i++)
	{
		current.v[0] = vertices[3*i];
		current.v[1] = vertices[3*i+1];
		current.v[2] = vertices[3*i+2];

		currentDist = getDist(worldPos,current);
		if(currentDist < closestDist)
		{
			closestDist = currentDist;
			closest = i; 
		}
	}
	
	closestPoint = vec3(vertices[3*closest],vertices[3*closest+1],vertices[3*closest+2]);
	// return the index of the closest vertex 
	return closestPoint; 
}
/*
	gets the distance between two points.
*/
float getDist(vec3 point, vec3 otherPoint)
{
	float dist; 

	float xDiff = point.v[0] - otherPoint.v[0];
	float yDiff = point.v[1] - otherPoint.v[1];
	float zDiff = point.v[2] - otherPoint.v[2];

	dist = sqrtf((xDiff*xDiff)+(yDiff*yDiff)+(zDiff*zDiff));

	return dist; 
}