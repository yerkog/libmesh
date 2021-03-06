// The libMesh Finite Element Library.
// Copyright (C) 2002-2020 Benjamin S. Kirk, John W. Peterson, Roy H. Stogner

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#include "elasticity_system.h"

#include "libmesh/dof_map.h"
#include "libmesh/elem.h"
#include "libmesh/fe_base.h"
#include "libmesh/fem_context.h"
#include "libmesh/zero_function.h"
#include "libmesh/dirichlet_boundaries.h"
#include "libmesh/quadrature.h"
#include "libmesh/unsteady_solver.h"
#include "libmesh/parallel_implementation.h" // set_union

using namespace libMesh;

boundary_id_type boundary_id_min_z = 0;
boundary_id_type boundary_id_min_y = 1;
boundary_id_type boundary_id_max_x = 2;
boundary_id_type boundary_id_max_y = 3;
boundary_id_type boundary_id_min_x = 4;
boundary_id_type boundary_id_max_z = 5;

#if LIBMESH_DIM > 2

void ElasticitySystem::init_data()
{
  _u_var = this->add_variable ("Ux", _fe_type);
  if (_dim > 1)
    _v_var = this->add_variable ("Uy", _fe_type);
  else
    _v_var = _u_var;
  if (_dim > 2)
    _w_var = this->add_variable ("Uz", _fe_type);
  else
    _w_var = _v_var;

  this->time_evolving(_u_var,2);
  this->time_evolving(_v_var,2);
  this->time_evolving(_w_var,2);

#ifdef LIBMESH_ENABLE_DIRICHLET

  std::set<boundary_id_type> all_boundary_ids, dirichlet_boundary_ids,
    dirichlet_u_boundary_ids, dirichlet_v_boundary_ids;
  all_boundary_ids = this->get_mesh().get_boundary_info().get_boundary_ids();
  this->comm().set_union(all_boundary_ids);

  if (all_boundary_ids.count(boundary_id_min_x))
    dirichlet_boundary_ids.insert(boundary_id_min_x);
  if (all_boundary_ids.count(node_boundary_id))
    dirichlet_boundary_ids.insert(node_boundary_id);
  if (all_boundary_ids.count(edge_boundary_id))
    dirichlet_boundary_ids.insert(edge_boundary_id);
  if (all_boundary_ids.count(fixed_u_boundary_id))
    dirichlet_u_boundary_ids.insert(fixed_u_boundary_id);
  if (all_boundary_ids.count(fixed_v_boundary_id))
    dirichlet_v_boundary_ids.insert(fixed_v_boundary_id);

  std::vector<unsigned int> variables, u_variable, v_variable;
  u_variable.push_back(_u_var);
  v_variable.push_back(_v_var);
  variables.push_back(_u_var);
  if (_dim > 1)
    variables.push_back(_v_var);
  if (_dim > 2)
    variables.push_back(_w_var);

  ZeroFunction<> zf;

  // Most DirichletBoundary users will want to supply a "locally
  // indexed" functor
  DirichletBoundary dirichlet_bc(dirichlet_boundary_ids, variables,
                                 zf, LOCAL_VARIABLE_ORDER);
  this->get_dof_map().add_dirichlet_boundary(dirichlet_bc);

  if (!dirichlet_u_boundary_ids.empty())
    {
      DirichletBoundary dirichlet_u_bc(dirichlet_u_boundary_ids,
                                       u_variable, zf,
                                       LOCAL_VARIABLE_ORDER);
      this->get_dof_map().add_dirichlet_boundary(dirichlet_u_bc);
    }

  if (!dirichlet_v_boundary_ids.empty())
    {
      DirichletBoundary dirichlet_v_bc(dirichlet_v_boundary_ids,
                                       v_variable, zf,
                                       LOCAL_VARIABLE_ORDER);
      this->get_dof_map().add_dirichlet_boundary(dirichlet_v_bc);
    }

#endif // LIBMESH_ENABLE_DIRICHLET

  // Do the parent's initialization after variables and boundary constraints are defined
  FEMSystem::init_data();
}

void ElasticitySystem::init_context(DiffContext & context)
{
  FEMContext & c = cast_ref<FEMContext &>(context);

  FEBase * u_elem_fe;
  FEBase * u_side_fe;

  c.get_element_fe(_u_var, u_elem_fe);
  c.get_side_fe(_u_var, u_side_fe);

  // We should prerequest all the data
  // we will need to build the residuals.
  u_elem_fe->get_JxW();
  u_elem_fe->get_phi();
  u_elem_fe->get_dphi();

  u_side_fe->get_JxW();
  u_side_fe->get_phi();

  // We might want to apply traction perpendicular to some boundaries.
  u_side_fe->get_normals();
}

bool ElasticitySystem::element_time_derivative(bool request_jacobian,
                                               DiffContext & context)
{
  FEMContext & c = cast_ref<FEMContext &>(context);

  // If we have an unsteady solver, then we need to extract the corresponding
  // velocity variable. This allows us to use either a FirstOrderUnsteadySolver
  // or a SecondOrderUnsteadySolver. That is, we get back the velocity variable
  // index for FirstOrderUnsteadySolvers or, if it's a SecondOrderUnsteadySolver,
  // this is actually just giving us back the same variable index.

  // If we only wanted to use a SecondOrderUnsteadySolver, then this
  // step would be unnecessary and we would just
  // populate the _u_var, etc. blocks of the residual and Jacobian.
  unsigned int u_dot_var = this->get_second_order_dot_var(_u_var);
  unsigned int v_dot_var = this->get_second_order_dot_var(_v_var);
  unsigned int w_dot_var = this->get_second_order_dot_var(_w_var);

  FEBase * u_elem_fe;
  c.get_element_fe(_u_var, u_elem_fe);

  // The number of local degrees of freedom in each variable
  const unsigned int n_u_dofs = c.n_dof_indices(_u_var);

  // Element Jacobian * quadrature weights for interior integration
  const std::vector<Real> & JxW = u_elem_fe->get_JxW();

  const std::vector<std::vector<Real>> & phi = u_elem_fe->get_phi();
  const std::vector<std::vector<RealGradient>> & grad_phi = u_elem_fe->get_dphi();

  // We set _w_var=_v_var etc in lower dimensions so this is sane:
  DenseSubVector<Number> & Fu = c.get_elem_residual(u_dot_var);
  DenseSubVector<Number> & Fv = c.get_elem_residual(v_dot_var);
  DenseSubVector<Number> & Fw = c.get_elem_residual(w_dot_var);

  DenseSubMatrix<Number> & Kuu = c.get_elem_jacobian(u_dot_var, _u_var);
  DenseSubMatrix<Number> & Kvv = c.get_elem_jacobian(v_dot_var, _v_var);
  DenseSubMatrix<Number> & Kww = c.get_elem_jacobian(w_dot_var, _w_var);
  DenseSubMatrix<Number> & Kuv = c.get_elem_jacobian(u_dot_var, _v_var);
  DenseSubMatrix<Number> & Kuw = c.get_elem_jacobian(u_dot_var, _w_var);
  DenseSubMatrix<Number> & Kvu = c.get_elem_jacobian(v_dot_var, _u_var);
  DenseSubMatrix<Number> & Kvw = c.get_elem_jacobian(v_dot_var, _w_var);
  DenseSubMatrix<Number> & Kwu = c.get_elem_jacobian(w_dot_var, _u_var);
  DenseSubMatrix<Number> & Kwv = c.get_elem_jacobian(w_dot_var, _v_var);

  unsigned int n_qpoints = c.get_element_qrule().n_points();

  Gradient body_force(0.0, 0.0, -1.0);

  for (unsigned int qp=0; qp != n_qpoints; qp++)
    {
      Gradient grad_u, grad_v, grad_w;
      c.interior_gradient(_u_var, qp, grad_u);
      if (_dim > 1)
        c.interior_gradient(_v_var, qp, grad_v);
      if (_dim > 2)
        c.interior_gradient(_w_var, qp, grad_w);

      // Convenience
      Tensor grad_U (grad_u, grad_v, grad_w);

      Tensor tau;
      for (unsigned int i = 0; i < _dim; i++)
        for (unsigned int j = 0; j < _dim; j++)
          for (unsigned int k = 0; k < _dim; k++)
            for (unsigned int l = 0; l < _dim; l++)
              tau(i,j) += elasticity_tensor(i,j,k,l)*grad_U(k,l);

      for (unsigned int i=0; i != n_u_dofs; i++)
        {
          for (unsigned int alpha = 0; alpha < _dim; alpha++)
            {
              Fu(i) += (tau(0,alpha)*grad_phi[i][qp](alpha) - body_force(0)*phi[i][qp])*JxW[qp];
              if (_dim > 1)
                Fv(i) += (tau(1,alpha)*grad_phi[i][qp](alpha) - body_force(1)*phi[i][qp])*JxW[qp];
              if (_dim > 2)
                Fw(i) += (tau(2,alpha)*grad_phi[i][qp](alpha) - body_force(2)*phi[i][qp])*JxW[qp];

              if (request_jacobian)
                {
                  for (unsigned int j=0; j != n_u_dofs; j++)
                    {
                      for (unsigned int beta = 0; beta < _dim; beta++)
                        {
                          // Convenience
                          const Real c0 = grad_phi[j][qp](beta)*c.get_elem_solution_derivative();

                          Real dtau_uu = elasticity_tensor(0, alpha, 0, beta)*c0;
                          Kuu(i,j) += dtau_uu*grad_phi[i][qp](alpha)*JxW[qp];

                          if (_dim > 1)
                            {
                              Real dtau_uv = elasticity_tensor(0, alpha, 1, beta)*c0;
                              Real dtau_vu = elasticity_tensor(1, alpha, 0, beta)*c0;
                              Real dtau_vv = elasticity_tensor(1, alpha, 1, beta)*c0;

                              Kuv(i,j) += dtau_uv*grad_phi[i][qp](alpha)*JxW[qp];
                              Kvu(i,j) += dtau_vu*grad_phi[i][qp](alpha)*JxW[qp];
                              Kvv(i,j) += dtau_vv*grad_phi[i][qp](alpha)*JxW[qp];
                            }
                          if (_dim > 2)
                            {
                              Real dtau_uw = elasticity_tensor(0, alpha, 2, beta)*c0;
                              Real dtau_vw = elasticity_tensor(1, alpha, 2, beta)*c0;
                              Real dtau_wu = elasticity_tensor(2, alpha, 0, beta)*c0;
                              Real dtau_wv = elasticity_tensor(2, alpha, 1, beta)*c0;
                              Real dtau_ww = elasticity_tensor(2, alpha, 2, beta)*c0;

                              Kuw(i,j) += dtau_uw*grad_phi[i][qp](alpha)*JxW[qp];
                              Kvw(i,j) += dtau_vw*grad_phi[i][qp](alpha)*JxW[qp];
                              Kwu(i,j) += dtau_wu*grad_phi[i][qp](alpha)*JxW[qp];
                              Kwv(i,j) += dtau_wv*grad_phi[i][qp](alpha)*JxW[qp];
                              Kww(i,j) += dtau_ww*grad_phi[i][qp](alpha)*JxW[qp];
                            }
                        }
                    }
                }
            }
        }

    } // qp loop

  // If the Jacobian was requested, we computed it. Otherwise, we didn't.
  return request_jacobian;
}

bool ElasticitySystem::side_time_derivative (bool request_jacobian,
                                             DiffContext & context)
{
  FEMContext & c = cast_ref<FEMContext &>(context);

  // If we're on the correct side, apply the traction
  if (c.has_side_boundary_id(traction_boundary_id) ||
      c.has_side_boundary_id(pressure_boundary_id))
    {
      // If we have an unsteady solver, then we need to extract the corresponding
      // velocity variable. This allows us to use either a FirstOrderUnsteadySolver
      // or a SecondOrderUnsteadySolver. That is, we get back the velocity variable
      // index for FirstOrderUnsteadySolvers or, if it's a SecondOrderUnsteadySolver,
      // this is actually just giving us back the same variable index.

      // If we only wanted to use a SecondOrderUnsteadySolver, then this
      // step would be unnecessary and we would just
      // populate the _u_var, etc. blocks of the residual and Jacobian.
      unsigned int u_dot_var = this->get_second_order_dot_var(_u_var);
      unsigned int v_dot_var = this->get_second_order_dot_var(_v_var);
      unsigned int w_dot_var = this->get_second_order_dot_var(_w_var);

      FEBase * u_side_fe;
      c.get_side_fe(_u_var, u_side_fe);

      // The number of local degrees of freedom in each variable
      const unsigned int n_u_dofs = c.n_dof_indices(_u_var);

      // We set _w_var=_v_var etc in lower dimensions so this is sane:
      DenseSubVector<Number> & Fu = c.get_elem_residual(u_dot_var);
      DenseSubVector<Number> & Fv = c.get_elem_residual(v_dot_var);
      DenseSubVector<Number> & Fw = c.get_elem_residual(w_dot_var);

      // Element Jacobian * quadrature weights for interior integration
      const std::vector<Real> & JxW = u_side_fe->get_JxW();

      const std::vector<std::vector<Real>> & phi = u_side_fe->get_phi();

      const std::vector<Point> & normals = u_side_fe->get_normals();

      unsigned int n_qpoints = c.get_side_qrule().n_points();

      Real pressure = 100;
      Gradient traction;
      traction(_dim-1) = -1;

      const bool pressureforce =
        c.has_side_boundary_id(pressure_boundary_id);

      for (unsigned int qp=0; qp != n_qpoints; qp++)
        {
          if (pressureforce)
            traction = pressure * normals[qp];

          for (unsigned int i=0; i != n_u_dofs; i++)
            {
              Fu(i) -= traction(0)*phi[i][qp]*JxW[qp];
              if (_dim > 1)
                Fv(i) -= traction(1)*phi[i][qp]*JxW[qp];
              if (_dim > 2)
                Fw(i) -= traction(2)*phi[i][qp]*JxW[qp];
            }
        }
    }

  // If the Jacobian was requested, we computed it (in this case it's zero).
  // Otherwise, we didn't.
  return request_jacobian;
}

bool ElasticitySystem::mass_residual(bool request_jacobian,
                                     DiffContext & context)
{
  FEMContext & c = cast_ref<FEMContext &>(context);

  // We need to extract the corresponding velocity variable.
  // This allows us to use either a FirstOrderUnsteadySolver
  // or a SecondOrderUnsteadySolver. That is, we get back the velocity variable
  // index for FirstOrderUnsteadySolvers or, if it's a SecondOrderUnsteadySolver,
  // this is actually just giving us back the same variable index.

  // If we only wanted to use a SecondOrderUnsteadySolver, then this
  // step would be unnecessary and we would just
  // populate the _u_var, etc. blocks of the residual and Jacobian.
  unsigned int u_dot_var = this->get_second_order_dot_var(_u_var);
  unsigned int v_dot_var = this->get_second_order_dot_var(_v_var);
  unsigned int w_dot_var = this->get_second_order_dot_var(_w_var);

  FEBase * u_elem_fe;
  c.get_element_fe(u_dot_var, u_elem_fe);

  // The number of local degrees of freedom in each variable
  const unsigned int n_u_dofs = c.n_dof_indices(u_dot_var);

  // Element Jacobian * quadrature weights for interior integration
  const std::vector<Real> & JxW = u_elem_fe->get_JxW();

  const std::vector<std::vector<Real>> & phi = u_elem_fe->get_phi();

  // Residuals that we're populating
  // We set _w_var=_v_var etc in lower dimensions so this is sane:
  libMesh::DenseSubVector<libMesh::Number> & Fu = c.get_elem_residual(u_dot_var);
  libMesh::DenseSubVector<libMesh::Number> & Fv = c.get_elem_residual(v_dot_var);
  libMesh::DenseSubVector<libMesh::Number> & Fw = c.get_elem_residual(w_dot_var);

  libMesh::DenseSubMatrix<libMesh::Number> & Kuu = c.get_elem_jacobian(u_dot_var, u_dot_var);
  libMesh::DenseSubMatrix<libMesh::Number> & Kvv = c.get_elem_jacobian(v_dot_var, v_dot_var);
  libMesh::DenseSubMatrix<libMesh::Number> & Kww = c.get_elem_jacobian(w_dot_var, w_dot_var);

  unsigned int n_qpoints = c.get_element_qrule().n_points();

  for (unsigned int qp=0; qp != n_qpoints; qp++)
    {
      // If we only cared about using FirstOrderUnsteadySolvers for time-stepping,
      // then we could actually just use interior rate, but using interior_accel
      // allows this assembly to function for both FirstOrderUnsteadySolvers
      // and SecondOrderUnsteadySolvers
      libMesh::Number u_ddot, v_ddot, w_ddot;
      c.interior_accel(u_dot_var, qp, u_ddot);
      if (_dim > 1)
        c.interior_accel(v_dot_var, qp, v_ddot);
      if (_dim > 2)
        c.interior_accel(w_dot_var, qp, w_ddot);

      for (unsigned int i=0; i != n_u_dofs; i++)
        {
          Fu(i) += _rho*u_ddot*phi[i][qp]*JxW[qp];
          if (_dim > 1)
            Fv(i) += _rho*v_ddot*phi[i][qp]*JxW[qp];
          if (_dim > 2)
            Fw(i) += _rho*w_ddot*phi[i][qp]*JxW[qp];

          if (request_jacobian)
            {
              for (unsigned int j=0; j != n_u_dofs; j++)
                {
                  libMesh::Real jac_term = _rho*phi[i][qp]*phi[j][qp]*JxW[qp];
                  jac_term *= context.get_elem_solution_accel_derivative();

                  Kuu(i,j) += jac_term;
                  if (_dim > 1)
                    Kvv(i,j) += jac_term;
                  if (_dim > 2)
                    Kww(i,j) += jac_term;
                }
            }
        }
    }

  // If the Jacobian was requested, we computed it. Otherwise, we didn't.
  return request_jacobian;
}
#endif // LIBMESH_DIM > 2

Real ElasticitySystem::elasticity_tensor(unsigned int i, unsigned int j, unsigned int k, unsigned int l)
{
  // Hard code material parameters for the sake of simplicity
  const Real poisson_ratio = 0.3;
  const Real young_modulus = 1.0e2;

  // Define the Lame constants
  const Real lambda_1 = (young_modulus*poisson_ratio)/((1.+poisson_ratio)*(1.-2.*poisson_ratio));
  const Real lambda_2 = young_modulus/(2.*(1.+poisson_ratio));

  return
    lambda_1 * kronecker_delta(i, j) * kronecker_delta(k, l) +
    lambda_2 * (kronecker_delta(i, k) * kronecker_delta(j, l) + kronecker_delta(i, l) * kronecker_delta(j, k));
}
