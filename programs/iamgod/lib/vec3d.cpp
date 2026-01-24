#include <vec3d.h>
#include <libmath.h>

Vector3D::Vector3D(float x, float y, float z)
{
	vec[0] = x;
	vec[1] = y;
	vec[2] = z;
}

Vector3D::Vector3D(const Vector4D& v)
{
	if (v.w() == 1) {
		vec[0] = v.x();
		vec[1] = v.y();
		vec[2] = v.z();
	} else {
		vec[0] = v.x() / v.w();
		vec[1] = v.y() / v.w();
		vec[2] = v.z() / v.w();
	}
}

float Vector3D::length() const
{
	return sqrt(x() * x() + y() * y() + z() * z());
}

Vector3D Vector3D::crossProduct(const Vector3D& v1, const Vector3D& v2)
{
	return Vector3D(v1[1] * v2[2] - v1[2] * v2[1], \
			v1[2] * v2[0] - v1[0] * v2[2], \
			v1[0] * v2[1] - v1[1] * v2[0]);
}

Vector3D& Vector3D::operator+=(const Vector3D& vector)
{
	setX(x() + vector.x());
	setY(y() + vector.y());
	setZ(z() + vector.z());
	return *this;
}

Vector3D& Vector3D::operator-=(const Vector3D& vector)
{
	setX(x() - vector.x());
	setY(y() - vector.y());
	setZ(z() - vector.z());
	return *this;
}

Vector3D& Vector3D::operator*=(const Vector3D& vector)
{
	setX(x() * vector.x());
	setY(y() * vector.y());
	setZ(z() * vector.z());
	return *this;
}

Vector3D& Vector3D::operator/=(const Vector3D& vector)
{
	setX(x() / vector.x());
	setY(y() / vector.y());
	setZ(z() / vector.z());
	return *this;
}

Vector3D& Vector3D::operator*=(float m)
{
	setX(x() * m);
	setY(y() * m);
	setZ(z() * m);
	return *this;
}

Vector3D& Vector3D::operator/=(float d)
{
	setX(x() / d);
	setY(y() / d);
	setZ(z() / d);
	return *this;
}

const Vector3D operator+(const Vector3D& v1, const Vector3D& v2)
{
	Vector3D v(v1);
	v += v2;
	return v;
}

const Vector3D operator-(const Vector3D& v1, const Vector3D& v2)
{
	Vector3D v(v1);
	v -= v2;
	return v;
}

const Vector3D operator*(const Vector3D& v1, const Vector3D& v2)
{
	Vector3D v(v1);
	v *= v2;
	return v;
}

const Vector3D operator/(const Vector3D& v1, const Vector3D& v2)
{
	Vector3D v(v1);
	v /= v2;
	return v;
}

const Vector3D operator-(const Vector3D& vector)
{
	Vector3D v;
	v -= vector;
	return v;
}

const Vector3D operator*(const Vector3D& v1, float m)
{
	Vector3D v(v1);
	v *= m;
	return v;
}

const Vector3D operator/(const Vector3D& v1, float d)
{
	Vector3D v(v1);
	v /= d;
	return v;
}